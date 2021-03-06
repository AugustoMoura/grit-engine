/* Copyright (c) David Cunningham and the Grit Game Engine project 2015
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "cache_friendly_range_space_simd.h"
#include "core_option.h"
#include "grit_class.h"
#include "main.h"
#include "streamer.h"

float streamer_visibility;
float streamer_prepare_distance_factor;
float streamer_fade_out_factor;
float streamer_fade_overlap_factor;

typedef CacheFriendlyRangeSpace<GritObjectPtr> Space;
static Space rs;

static GObjPtrs activated;
static GObjPtrs loaded;
static GObjPtrs fresh; // just been added - skip the queue for activation

typedef std::vector<StreamerCallback*> StreamerCallbacks;
StreamerCallbacks streamer_callbacks;

static void remove_if_exists (GObjPtrs &list, const GritObjectPtr &o)
{
    GObjPtrs::iterator iter = find(list.begin(),list.end(),o);
    if (iter!=list.end()) {
        size_t offset = iter - list.begin();
        list[offset] = list[list.size()-1];
        list.pop_back();
    }
}

void streamer_init (void)
{
}


void streamer_centre (lua_State *L, const Vector3 &new_pos, bool everything)
{
        int step_size = everything ? INT_MAX : core_option(CORE_STEP_SIZE);

        Space::Cargo fnd = fresh;
        fresh.clear();

        const float visibility = streamer_visibility;

        const float pF = streamer_prepare_distance_factor;
        const float tpF = pF * visibility; // prepare and visibility factors
        const float vis2 = visibility * visibility;

        typedef GObjPtrs::iterator I;

        // iterate through, deactivating things


        ////////////////////////////////////////////////////////////////////////
        // DEACTIVATE DISTANT GRIT OBJECTS /////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////
        // use victims because deactivate() changes the 'activated' list
        // and so does notifyRange2 if the callback raises an error
        GObjPtrs victims = activated;
        for (I i=victims.begin(), i_=victims.end() ; i!=i_ ; ++i) {
                const GritObjectPtr &o = *i;
                 //note we use vis2 not visibility
                float range2 = o->range2(new_pos) / vis2;
                // sometimes deactivation of an object can cause the deletion of other objects
                // if those objects are also in the victims list, this can become a problem
                // so just skip them
                if (o->getClass()==NULL) continue;
                o->notifyRange2(L,o,range2);
                const GritObjectPtr &the_far = o->getFarObj();
                if (!the_far.isNull()) {
                        // update the far (perhaps for a second time this frame)
                        // to make sure it has picked up the fade imposed by o
                        float range2 = the_far->range2(new_pos) / vis2;
                        the_far->notifyRange2(L,the_far,range2);
                }
                if (range2 > 1) {
                        // now out of range
                        const GritObjectPtr &o = *i;
                        const GritObjectPtr &the_far = o->getFarObj();
                        bool killme = o->deactivate(L,o);
                        if (!the_far.isNull()) {
                                // we're deactivating and we have a far,
                                // so make sure it gets considered this frame
                                fnd.push_back(the_far);
                        }
                        if (killme) {
                                object_del(L,o);
                        }
                }
        }

        ////////////////////////////////////////////////////////////////////////
        // UNLOAD RESOURCES FOR VERY DISTANT GRIT OBJECTS //////////////////////
        ////////////////////////////////////////////////////////////////////////
        for (size_t i=0, i_=loaded.size(); i<i_ ;) {
                // Iteration should be fast for removal of significant number of
                // elements.  Don't do this if it ever stops being a vector.
                const GritObjectPtr &o = loaded[i];
                if (!o->withinRange(new_pos,tpF)) {
                        // unregister demand...
                        // we deactivated first so this should
                        // unload any resources we were using
                        o->tryUnloadResources();
                        loaded[i] = loaded[i_-1];
                        loaded.pop_back();
                        --i_;
                } else {
                        ++i;
                }
        }


        GObjPtrs must_kill;

        ////////////////////////////////////////////////////////////////////////
        // LOAD RESOURCES FOR APPROACHING GRIT OBJECTS /////////////////////////
        // AND... ACTIVATE ARRIVING GRIT OBJECTS ///////////////////////////////
        ////////////////////////////////////////////////////////////////////////
        // note: since fnd is prepopulated by new objects and the lods of deactivated objects
        // it may have duplicates after the rangespace has gone through
        rs.getPresent(new_pos.x, new_pos.y, new_pos.z, step_size, tpF, fnd);
        for (Space::Cargo::iterator i=fnd.begin(),i_=fnd.end() ; i!=i_ ; ++i) {
                const GritObjectPtr &o = *i;

                // this can happen if there is a duplicate in the list and it gets destroyed
                // the first time due to an error in the activation function
                // but is eventually processed a second time after being destroyed
                if (o->getClass()==NULL) continue;
                if (o->isActivated()) continue;

                float range2 = o->range2(new_pos) / vis2;
                // not in range yet
                if (range2 > 1) continue;

                //CVERB << "Preparing for activation: " << o->name << std::endl;

                // consider background loading
                if (o->backgroundLoadingCausedError()) {
                        must_kill.push_back(o);
                        continue;
                }
                if (o->requestLoad(new_pos)) {
                        // this means we weren't already in the queue.
                        // we may still not be in the queue, if all resources are loaded

                        // note 'loaded' includes things which have started
                        // but not finished loading...
                        loaded.push_back(o);
                }
                if (o->isInBackgroundQueue()) {
                        // if we're in the queue we probably have to wait longer
                        // before all the resources are loaded
                        continue;
                }
                if (o->backgroundLoadingCausedError()) {
                        must_kill.push_back(o);
                        continue;
                }

                // if we get this far, we should be displayed but there might be
                // a near object in the way
                GritObjectPtr the_near = o->getNearObj();
                while (!the_near.isNull()) {
                        if (the_near->withinRange(new_pos,visibility * streamer_fade_overlap_factor)) {
                                if (the_near->isActivated()) {
                                        // why deactivate?
                                        // we already ensured it is not activated above...
                                        o->deactivate(L,o);
                                        // don't activate, near gobj is
                                        // in the way
                                        goto skip;
                                }
                        }
                        the_near = the_near->getNearObj();
                }
      

                // ok there wasn't so activate
                o->activate(L,o);

                // activation can result in a lua error which triggers the destruction of the
                // object 'o' so we test for that here before doing more stuff
                if (o->getClass()==NULL) continue;

                o->notifyRange2(L,o,range2);

                skip:;
        }

        for (GObjPtrs::iterator i=must_kill.begin(),i_=must_kill.end() ; i!=i_ ; ++i) {
                CERR << "Object: \"" << (*i)->name << "\" raised an error while background loading resources, so destroying it." << std::endl;
                object_del(L, *i);
        }

        bgl->handleBastards();
        bgl->checkRAMHost();
        bgl->checkRAMGPU();

        for (StreamerCallbacks::iterator i=streamer_callbacks.begin(),i_=streamer_callbacks.end() ; i!=i_ ; ++i) {
                (*i)->update(new_pos);
        }

}

void streamer_update_sphere (size_t index, const Vector3 &pos, float d)
{
    rs.updateSphere(index,pos.x,pos.y,pos.z,d);
}

void streamer_object_activated (GObjPtrs::iterator &begin, GObjPtrs::iterator &end)
{
    begin = activated.begin();
    end = activated.end();
}

int streamer_object_activated_count (void)
{
    return activated.size();
}

void streamer_list(const GritObjectPtr &o)
{
    rs.add(o);
    fresh.push_back(o);
}

void streamer_unlist(const GritObjectPtr &o)
{
    rs.remove(o);
    remove_if_exists(fresh, o);
}

void streamer_list_as_activated (const GritObjectPtr &o)
{
    GObjPtrs::iterator begin = activated.begin(), end = activated.end();
    GObjPtrs::iterator iter  = find(begin,end,o);
    if (iter!=end) return;
    activated.push_back(o);
}

void streamer_unlist_as_activated (const GritObjectPtr &o)
{
    GObjPtrs::iterator begin = activated.begin(), end = activated.end();
    GObjPtrs::iterator iter  = find(begin,end,o);
    if (iter==end) return;
    size_t index = iter - begin;
    activated[index] = activated[activated.size()-1];
    activated.pop_back();
}


void streamer_callback_register (StreamerCallback *rc)
{
    StreamerCallbacks::iterator begin = streamer_callbacks.begin(), end = streamer_callbacks.end();
    StreamerCallbacks::iterator iter  = find(begin,end,rc);
    if (iter!=end) return;
    streamer_callbacks.push_back(rc);
}

void streamer_callback_unregister (StreamerCallback *rc)
{
    StreamerCallbacks::iterator begin = streamer_callbacks.begin(), end = streamer_callbacks.end();
    StreamerCallbacks::iterator iter  = find(begin,end,rc);
    if (iter==end) return;
    size_t index = iter - begin;
    streamer_callbacks[index] = streamer_callbacks[streamer_callbacks.size()-1];
    streamer_callbacks.pop_back();
}

// vim: shiftwidth=4:tabstop=4:expandtab
