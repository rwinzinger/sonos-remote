// Every Sonos call in the project goes through this file. The local UPnP API is
// unofficial, so when it changes this is the single place to patch (CLAUDE.md).
//
// Transport + state only: no encoder logic, no UI, no rate limiting. Callers coalesce
// their own turns before calling the volume functions.
//
// Validated against the real system: endpoint paths, the quoted SOAPAction header, the
// InstanceID=0 envelope, the <CurrentVolume> reply, and GetZoneGroupState's
// Coordinator / ZoneGroupMember layout.

#pragma once

#include <Arduino.h>

namespace sonos {

const uint8_t MAX_ZONES = 8;

enum class ResolveResult {
  Ok,
  NoPlayersFound,     // SSDP returned nothing — wrong network, or WiFi not up
  NoTopology,         // players answered SSDP but not GetZoneGroupState
  RoomNotFound,       // topology fine, but no group contains the requested room
};

struct Zone {
  String room;            // ZoneName, e.g. "Main"
  String uuid;            // RINCON_...
  String ip;
  String groupCoordUuid;  // coordinator of the group this zone belongs to
  bool   invisible;       // bonded satellite (stereo RF, surround, sub) — never target it
};

void begin(const char *preferredIp, const char *targetRoom);

// Full resolve: SSDP discovery + topology. Costs ~1.5 s — first run and recovery only.
ResolveResult resolveCoordinator();

// Cheap re-read of the topology from a already-known player (single POST, ~30 ms). Falls
// back to a full resolveCoordinator() only if that player has gone away. Use this for
// routine refreshes — never call resolveCoordinator() on a timer.
bool refreshTopology();

// --- zone table -----------------------------------------------------------------------
uint8_t zoneCount();
const Zone *zoneAt(uint8_t i);
const Zone *zoneByRoom(const char *room);          // first VISIBLE zone with that name
const Zone *coordinatorForRoom(const char *room);  // the zone leading that room's group

// True when both rooms currently sit in the same group.
bool roomsGrouped(const char *roomA, const char *roomB);

// --- volume ---------------------------------------------------------------------------
bool getGroupVolume(int &volumeOut);                       // target room's group
bool setRelativeGroupVolume(int adjustment, int &newVolumeOut);

// A single room's OWN volume (RenderingControl, not GroupRenderingControl). Needed because
// GetGroupVolume returns the AVERAGE across group members — verified live: Main 13 and
// Stereo 25 report a group volume of 19 — so the group number alone hides how far apart the
// rooms are set.
bool getRoomVolume(const char *room, int &volumeOut);

// Requirement 1: apply the SAME relative change to EVERY distinct group, so the knob works
// whether the rooms are joined or separate. Returns the number of groups successfully
// adjusted, and reports the target room's new level in volumeOut when known.
uint8_t adjustVolumeAllGroups(int adjustment, int &volumeOut);

// --- grouping (requirement 3) ----------------------------------------------------------
// Join `room` into `targetRoom`'s group.
bool joinRoomTo(const char *room, const char *targetRoom);

// Detach `room` into its own standalone group. If it currently COORDINATES a group with
// other members, coordination is handed over first via DelegateGroupCoordinationTo so the
// remaining speakers keep playing — otherwise the stream dies with the departing player.
bool detachRoom(const char *room);

// --- line-in (requirement 2) -----------------------------------------------------------
// Switch `room` to its own analogue input and start playback. On the Era 300 this is the
// "Plattenspieler" input.
bool playLineIn(const char *room);

// Is line-in the current source for `room`?
bool isLineInActive(const char *room, bool &activeOut);

// --- transport --------------------------------------------------------------------------
bool isPlaying(const char *room, bool &playingOut);

const char *coordinatorIp();
const char *coordinatorRoom();
bool haveCoordinator();

// True when SONOS_TARGET_ROOM is not in the topology (its speaker is off) and the module
// is steering some other group instead. The UI should say so rather than pretend.
bool targetRoomMissing();
const char *resolveResultText(ResolveResult r);

}  // namespace sonos
