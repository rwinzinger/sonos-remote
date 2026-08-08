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

// Tell the module which room name refers to the stereo PAIR. Lookups for that name then
// fall back to the pair's stable UUID when the name no longer matches — which happens
// permanently after the first split, because Sonos reverts the pair's name and re-pairing
// does not restore it.
void setStereoRoomName(const char *room);

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

// Adjust ONE room's own volume (RenderingControl SetRelativeVolume). Used by Screen 2 when
// a single room is selected, so the dial moves only that room.
bool setRelativeRoomVolume(const char *room, int adjustment, int &newVolumeOut);

// Set ONE room to an absolute level. Used by Screen 2's Sync action.
bool setRoomVolume(const char *room, int volume);

// Requirement 1: apply the SAME relative change to EVERY room, so the knob works whether
// the rooms are joined or separate AND absolute differences between rooms are preserved.
//
// Deliberately per-ROOM, not SetRelativeGroupVolume. Sonos remembers a group's internal
// volume BALANCE separately from the members' actual levels: setting one member directly
// (as Sync does) does not update that balance, so the next group-relative call re-imposes
// the OLD ratio and silently undoes the sync. Measured: after syncing both rooms to 18, a
// single group +2 produced Main 18 / Stereo 22 — Main did not move at all.
//
// Per-room SetRelativeVolume never consults that stored balance, so equal stays equal and
// any intentional offset is preserved exactly.
//
// Returns how many rooms were adjusted; volumeOut receives the mean of their new levels
// (which equals the Sonos group volume when the rooms are grouped).
uint8_t adjustVolumeAllRooms(int adjustment, int &volumeOut);

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

// --- stereo pair (Era 100 L/R) ---------------------------------------------------------
// The pair can be split and rebuilt over UPnP: DeviceProperties exposes SeparateStereoPair
// and CreateStereoPair. Splitting turns the RIGHT speaker into a normal addressable zone.
//
// This is a CONFIGURATION change, not a playback one: it takes a few seconds and the pair
// visibly disappears/reappears in the Sonos app. The ChannelMapSet needed to rebuild the
// pair is only present in the topology WHILE paired, so it is cached in NVS — otherwise a
// split pair could not be restored after a reboot.
bool stereoPairKnown();        // a ChannelMapSet has been seen or restored from NVS
bool stereoPairSeparated();    // the right speaker is currently its own visible zone
bool separateStereoPair();
bool createStereoPair();         // also restores the pair's name (see setStereoRoomName)

// Restore the pair's zone name. Called automatically by createStereoPair(); exposed for
// repairing a pair that was split by something other than this firmware.
bool renameStereoPair(const char *name);
const char *rightSpeakerRoom();  // room name of the split-off right speaker, "" if paired
const char *rightSpeakerUuid();  // stable identity — names are unreliable after a split
bool stopAllExcept(const char *keepUuid);   // silence everything but that zone

// --- transport --------------------------------------------------------------------------
bool isPlaying(const char *room, bool &playingOut);

// True when the room is playing a Bluetooth / virtual line-in source. Used to recognise the
// TV setup even when the stereo pair is still bonded.
bool isBluetoothActive(const char *room, bool &activeOut);

// Transport of a specific zone by UUID. Needed for the split-off RIGHT speaker, which has
// no reliable room name of its own once the pair is separated.
bool isPlayingUuid(const char *uuid, bool &playingOut);

// "Now playing" for the room's GROUP, written into `out`.
//
// Always asks the group COORDINATOR: a follower reports CurrentURI "x-rincon:<coord>" and
// carries NO metadata, so querying the room directly returns nothing while music plays.
// Falls back to naming the source (Plattenspieler / Bluetooth / AirPlay / station) when a
// source has no track information. Returns false when nothing is playing.
bool getNowPlaying(const char *room, char *out, size_t outLen);
bool stopRoom(const char *room);
bool playUriOn(const char *room, const char *uri);   // e.g. a radio stream

const char *coordinatorIp();
const char *coordinatorRoom();
bool haveCoordinator();

// True when SONOS_TARGET_ROOM is not in the topology (its speaker is off) and the module
// is steering some other group instead. The UI should say so rather than pretend.
bool targetRoomMissing();
const char *resolveResultText(ResolveResult r);

}  // namespace sonos
