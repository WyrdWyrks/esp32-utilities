#pragma once

#include <cstring>

#include <Packet.h>
#include <helpers/SimpleMeshTables.h>

#include "LoraUtilities.hpp"   // LoraModule::Utilities::IncrementEchoCount

namespace LoraModule
{
    // SimpleMeshTables provides bounded packet-hash dedup: a fixed 160-entry
    // circular buffer of 8-byte hashes (MAX_PACKET_HASHES * MAX_HASH_SIZE),
    // replacing the old unbounded std::unordered_map<sender,msgID>.
    //
    // This subclass adds the "N echoes" counter the UI shows. MeshCore marks a
    // node's own outbound packet as seen *before* it leaves
    // (Mesh::sendFlood -> _tables->markSeen), so every rebroadcast of that
    // packet that comes back to us returns wasSeen() == true, starting with the
    // very first echo. setOwnOutbound() records the hash of the local user's own
    // ping right before it is sent; each later dedup hit against that hash bumps
    // the echo count.
    //
    // Minor: if 160 other packets pass through between our send and an echo, the
    // entry is evicted from the ring and that echo is missed. Irrelevant at this
    // traffic level.
    class MeshTables : public SimpleMeshTables
    {
    public:
        // Call on the Packet created for the local user's own ping, before
        // Mesh::sendFlood().
        void setOwnOutbound(const mesh::Packet* p)
        {
            p->calculatePacketHash(_own);
            _hasOwn = true;
        }

        bool wasSeen(const mesh::Packet* packet) override
        {
            bool seen = SimpleMeshTables::wasSeen(packet);
            if (seen && _hasOwn)
            {
                uint8_t h[MAX_HASH_SIZE];
                packet->calculatePacketHash(h);
                if (memcmp(h, _own, MAX_HASH_SIZE) == 0)
                {
                    LoraModule::Utilities::IncrementEchoCount();
                }
            }
            return seen;
        }

    private:
        uint8_t _own[MAX_HASH_SIZE] {};
        bool    _hasOwn = false;
    };
}
