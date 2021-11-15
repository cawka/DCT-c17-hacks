#ifndef MBPS_HPP
#define MBPS_HPP
/*
 * mbps.hpp: message-based pub/sub API for DCT (NDN network layer)
 *
 * Copyright (C) 2020 Pollere, Inc
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere, Inc at info@pollere.net.
 *
 *  This proof-of-concept is not intended as production code.
 *  More information on DCT is available from info@pollere.net
 */

#include <algorithm>
#include <bitset>
#include <functional>
#include <getopt.h>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <chrono>

//if not using syncps defaults, set these here
static constexpr size_t MAX_CONTENT=768; //max content size in bytes, <= maxPubSize in syncps.hpp
static constexpr size_t MAX_SEGS = 64;  //max segments of a msg, <= maxDifferences in syncps.hpp

#include "dct/syncps/syncps.hpp"
#include "dct/schema/dct_model.hpp"

using namespace syncps;

/* 
 * MBPS (message-based publish/subscribe) provides a pub/sub
 * data-centric transport inspired by the MQTT API.
 * MBPS uses the DCT run-time library, Operant's ndn-ind library, and Pollere's
 * patches to NFD.
 *
 * Messages passed from the application may exceed the size of
 * the Publications passed between the shim and syncps. Larger messages
 * are segmented and sent in multiple Publications and reassembled into
 * messages that are passed to the application's callback.
 *
 */

struct mbps;

using mbpsPub = DCTmodel::sPub;
// Used to pass message information to app that is not in message body
struct mbpsMsg : mbpsPub {
    using mbpsPub::mbpsPub;
    mbpsMsg(const mbpsPub& p) { *this = reinterpret_cast<const mbpsMsg&>(p); }
    mbpsMsg(mbpsPub&& p) { *this = std::move(reinterpret_cast<mbpsMsg&&>(p)); }
//    bool dup{0};
};
//publication parameter tags and values are passed to mbps in a vector of parItem pairs
// (defined in library as a string and a value that is a legal parmeter type)
using msgParms = std::vector<parItem>;
using msgHndlr = std::function<void(mbps&, const mbpsMsg&, std::vector<uint8_t>&)>;
using connectCb = std::function<void()>;
using confHndlr = std::function<void(const bool, const uint32_t)>;

using error = std::runtime_error;
using MsgID = uint32_t;
using SegCnt = uint16_t;
using Timer = ndn::scheduler::ScopedEventId;
using TimerCb = std::function<void()>;
using MsgInfo = std::unordered_map<MsgID,std::bitset<64>>;
using MsgSegs = std::vector<uint8_t>;
using MsgCache = std::unordered_map<MsgID,MsgSegs>;

struct mbps
{   
    connectCb m_connectCb;
    connectCb m_connFailCb;
    DCTmodel m_pb;
    Name m_pubpre{};        // full prefix for Publications
    std::string m_uniqId{};   //create this from #chainInfo to use in creating message Ids
    std::unordered_map<MsgID, confHndlr> m_msgConfCb;
    MsgInfo m_pending{};    // unconfirmed published messages
    MsgInfo m_received{};   //received publications of a message
    MsgCache m_reassemble{}; //reassembly of received message segments
    Timer m_timer;

    mbps(std::string_view bootstrap) : m_pb(bootstrap), m_pubpre{m_pb.pubPrefix()}  { }

    void run() { m_pb.run(); }
    const auto& pubPrefix() const noexcept { return m_pubpre; } //calling can convert to Name

    // relies on trust schema using mbps conventions of collecting all the signing chain
    // identity information (_role, _roleId, _room, etc.) in pseudo-pub "#chainInfo" so
    // the app can extract what it needs to operate.
    auto attribute(std::string_view v) const { return m_pb.pubVal("#chainInfo", v); }

    bool alwaysOn() { return true;} //should set to false for device types that sleep

    /*
     * Kicks off the set up necessary for an application to publish or receive
     * publications.  A client is considered "connected" once communications are
     * initialized which may include key distribution and/or acquisition. The callback
     * should be how the application starts its work that involves communication.
     * If m_pb.start results in a callback indicating success, m_connectCb is
     * invoked. If failure is indicated, m_connFailCb, which defaults to a
     * simple exit, is invoked.
     *
     * This is loosely analogous to MQTT's connect() which connects to a server,
     * but MBPS is serverless; this simply makes a client "ready" to communicate.
     *
     * connect does not timeout; if there is a wait time limit meaningful to an
     * application it should set its own timeout.
     */

    void connect(connectCb&& scb, connectCb&& fcb = [](){exit(1);})
    {
        //libsodium set up
        if (sodium_init() == -1) throw error("Connect unable to set up libsodium");
        m_connFailCb = std::move(fcb);
        m_connectCb = std::move(scb);
        m_uniqId = m_pb.pubVal("#chainInfo").toUri();

        // call start() with lambda to confirm success/failure
        // A second, optional argument can pass a function that returns a boolean
        // indicator of whether this entity can be used to make group keys. (Here, using
        // an indicator of whether the device is "always on.") Defaults to true.
        m_pb.start([this](bool success) {
                if(!success) {
                    _LOG_ERROR("mbps failed to initialize connection");
                    m_connFailCb();
                } else {
                    _LOG_INFO("mbps connect successfully initialized connection");
                    m_connectCb();
                }
            }, [this](){ return alwaysOn();}
            );
    }

    /*
     * Subscribe to all topics in the sync Collection with a single callback.
     *
     * An incoming Publication will cause cause the lambda to invoke
     * receivePub() with the Publication and the application's msgHndlr callback
    */
    mbps& subscribe(const msgHndlr& mh)    {
        _LOG_INFO("mbps:subscribe: single callback for client topic " << m_pubpre);
        m_pb.subscribeTo(pubPrefix(), [this,mh](auto p) {receivePub(p, mh);});
        return *this;
    }
    // distinguish subscriptions further by topic or topic/location
    mbps& subscribe(const std::string& suffix, const msgHndlr& mh)    {
        auto target = pubPrefix().toUri() + "/" + suffix;
        _LOG_INFO("mbps:subscribe set up subscription to target: " << target);
        m_pb.subscribeTo(target, [this,mh](auto p) {receivePub(p, mh);});
        return *this;
    }

    /*
     * receivePub() is called when a new Publication (carrying a message segment) is
     * received in a subscribed topic.
     *
     * A message is uniquely identified by its msgID and its timestamp.
     * and each name is identical except for the k in the k out of n sCnt.
     * When all n pieces received,reassemble into a message and callback
     * the message handler associated with subscription.
     * paramNames() gets the paramater tags of a publication while tagNames()
     * returns all the tags of a publication
     *
     * This receivePub guarantees in-order delivery of Publications within a message.
     *
     * If in-order delivery is required across messages from an origin for a particular
     * application, messages can be held by their origin and timestamp until ordering can
     * be determined or an additional sequence number can be introduced.
     */
     void receivePub(const Publication& pub, const msgHndlr& mh)
     {      
        const auto& p = mbpsPub(pub);
        //all the publication name ftags (in order) set by app or mbps
        SegCnt k = p.number("sCnt"), n = 1u;
        std::vector<uint8_t> msg{}; //for message body

        if (k == 0) { //single publication in this message
            if(auto sz = p.getContent().size())
                msg.assign(p.getContent().buf(), p.getContent().buf() + sz);
        } else {
            MsgID mId = p.number("msgID");
            n = 255 & k;    //bottom byte
            k >>= 8;
            if (k > n || k == 0 || n > MAX_SEGS) {
                _LOG_WARN("receivePub: msgID " << p.number("msgID") << " piece " << k << " > " << n << " pieces");
                return;
            }
            //reassemble message            
            const auto& m = *p.getContent();
            auto& dst = m_reassemble[mId];
            if (k == n)
                dst.resize((n-1)*MAX_CONTENT+m.size());
            else if (dst.size() == 0)
                dst.resize(n*MAX_CONTENT);
            std::copy(m.begin(), m.end(), dst.begin()+(--k)*MAX_CONTENT);
            m_received[mId].set(k);
            if (m_received[mId].count() != n) return; // all segments haven't arrived
            msg = m_reassemble[mId];
            m_received.erase(mId);  //delete msg state
            m_reassemble.erase(mId);
        }                
        /*
         * Complete message received, prepare arguments for msgHndlr callback
         */
        _LOG_INFO("receivePub: msgID " << p.number("msgID") << "(" << n << " pieces) delivered in " << p.timeDelta("mts") << " sec.");

        mh(*this, mbpsMsg(p), msg);
    }

    /*
     * Confirms whether Publication made it to the Collection.
     * If "at least once" semantics are desired, the confirmPublication
     * method is passed to syncps as the onPublished callback to indicate
     * if Publication was externally published or timed out.
     *
     * success = true means appeared in some other node's IBLT
     * false = Publication timed out without appearing in another node's IBLT.
     *
     * When all k of n segments are confirmed published, may invoke a
     * confirmMessage callback to the application that is set when message is
     * passed to shim. (a confHndlr callback)
     *
     */
    void confirmPublication(const Publication& pub, bool success)
    {
        const mbpsPub& p = mbpsPub(pub);
        MsgID mId = p.number("msgID");
        SegCnt k = p.number("sCnt"), n = 1u;
        if (k != 0) {
            // Don't need to keep state for single piece msgs but multi-piece succeed
            // only if all their pieces arrive and fail otherwise. Keep per-msg arrival
            // state in a bitmap that's created on-demand and erased on completion or timeout.
            n = k & 255;
            if (success) {
                m_pending[mId].set(k >> 8);
                if (m_pending[mId].count() != n) return; // all pieces haven't arrived
            }
            // either msg complete or piece timed out so delivery has failed - delete msg state
            k = m_pending[mId].count();
            if (m_pending.count(mId)) m_pending.erase(mId);
        }
        if (success) {  //TTP = "time to publish"
            _LOG_INFO("confirmPublication: msgID " << mId << "(" << n << " pieces) arrived, TTP " << p.timeDelta("mts"));
            //if a confirmation cb set by app, would go here
        } else {
            _LOG_INFO("confirmPublication: msgID " << mId << " " << n - k << " pieces (of " << n << ") timed out");
        }
        try {
            m_msgConfCb.at(mId)(success,mId);
        } catch (...) {
                //no confCb for this message, do nothing
        }
        m_msgConfCb.erase(mId); //used the callback so erase
    }

    /*
     * Publish the passed in message by building mbpsPubs to carry the message
     * content and passing to m_pb to publish
     *
     * An application calls this method and passes a vector of pairs (msgParms) that
     * is used to fill in any needed tag values and an optional message body.
     * (Might want to use paramNames to make sure all the parameter tags have been set.)
     *
     * The message may need to be broken into content-sized segments.
     * Publications for all segments have the same message ID and timestamp.
     * mId uniquely identifies using uint32_t hash of (origin, timestamp, message)
     * where <origin> is a tag value or combination of tag values the application
     * uses as unique identifiers (e.g., role/roleId)
     *
     * For messages with a confirmation callback (roughly mqtt QoS 1) set, a cb
     * is set in m_pb.publish to confirm each publication of msg and the app
     * callback function (a confHndlr) gets called either when all segments of a
     * message were published or if any segments timed out without showing up in
     * the off-node collection. An application may take action based on this.
     *
     * Adds the mbps-specific parameters to pass to publication builder
     *
     * Return message id if successful, 0 otherwise.
     */

    MsgID publish(msgParms&& mp, tcb::span<uint8_t> msg = {}, const confHndlr&& ch = nullptr)
    {
        /*
         * Set up and publish Publication(s)
         * can check here for required arguments
         * msgID is an uint32_t hash of the message, incorporating ID and timestamp to make unique
         */
        auto size = msg.size();
        auto mts = std::chrono::system_clock::now();
        mp.emplace_back("mts", mts);

        uint64_t tms = std::chrono::duration_cast<std::chrono::microseconds>(mts.time_since_epoch()).count();
        std::vector<uint8_t> emsg;
        for(size_t i=0; i<sizeof(tms); i++)
            emsg.push_back( tms >> i*8 );
        emsg.insert(emsg.end(), m_uniqId.begin(), m_uniqId.end());
        emsg.insert(emsg.end(), msg.begin(),msg.end());
        std::array<uint8_t, 4> h;        //so fits in uint32_t
        crypto_generichash(h.data(), h.size(), emsg.data(), emsg.size(), NULL, 0);
        uint32_t mId = h[0] | h[1] << 8 | h[2] << 16 | h[3] << 24;
        mp.emplace_back("msgID", mId);

        // determine number of message segments: sCnt forces n < 256,
        // iblt is sized for 80 but 64 fits in an int bitset
        size_t n = (size + (MAX_CONTENT - 1)) / MAX_CONTENT;
        if(n > MAX_SEGS) throw error("publishMsg: message too large");
        auto sCnt = n > 1? n + 256 : 0;
        mp.emplace_back("sCnt", sCnt);

        if(size == 0) { //empty message body
            if(ch)
                m_pb.publish(m_pb.pub({}, mp), [this](auto p, bool s) { confirmPublication(mbpsPub(p),s); });
            else
                m_pb.publish(m_pb.pub({}, mp));
            return mId;
        }

        // publish as many segments as needed
        for (auto off = 0u; off < size; off += MAX_CONTENT) {
            auto len = std::min(size - off, MAX_CONTENT);
            if(ch) {
                m_pb.publish(m_pb.pub(msg.subspan(off, len), mp),
                                  [this](auto p, bool s) { confirmPublication(mbpsPub(p),s); });
            } else {
                m_pb.publish(m_pb.pub(msg.subspan(off, len), mp));
            }
            sCnt += 256;    //segment names differ only in sCnt
            mp.pop_back();   //sCnt is last argument on the list
            mp.emplace_back("sCnt", sCnt);
        }
        if(ch) {
            _LOG_INFO("mbps has published (with call back) mId: " << mId);
            m_msgConfCb[mId] = std::move(ch);    //set mesg confirmation callback
        }
        return mId;
    }

    // Can be used by application to schedule
    Timer schedule(std::chrono::nanoseconds d, const TimerCb& cb) {
        return m_pb.schedule(d, cb);
    }
};

#endif
