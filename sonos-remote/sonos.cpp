#include "sonos.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>

namespace sonos {
namespace {

const uint16_t SONOS_PORT      = 1400;
const uint32_t SSDP_WAIT_MS    = 1500;   // only paid on first resolve / full rediscovery
const uint16_t HTTP_TIMEOUT_MS = 2000;   // a dead player must not stall the UI for 5 s

const char *PATH_GROUP_RENDERING = "/MediaRenderer/GroupRenderingControl/Control";
const char *PATH_AVTRANSPORT     = "/MediaRenderer/AVTransport/Control";
const char *PATH_RENDERING       = "/MediaRenderer/RenderingControl/Control";
const char *PATH_TOPOLOGY        = "/ZoneGroupTopology/Control";
const char *SVC_GROUP_RENDERING  = "urn:schemas-upnp-org:service:GroupRenderingControl:1";
const char *SVC_AVTRANSPORT      = "urn:schemas-upnp-org:service:AVTransport:1";
const char *SVC_RENDERING        = "urn:schemas-upnp-org:service:RenderingControl:1";
const char *SVC_TOPOLOGY         = "urn:schemas-upnp-org:service:ZoneGroupTopology:1";

String preferredIp_;
String targetRoom_;
String coordinatorIp_;
String coordinatorRoom_;

// Any reachable player can serve the whole topology, so remember one and reuse it. This is
// what turns a routine refresh from a 1.5 s SSDP sweep into a single ~30 ms POST.
String lastGoodIp_;

// True when the configured target room is absent from the topology and we are steering a
// different group instead.
bool targetMissing_ = false;

Zone    zones_[MAX_ZONES];
uint8_t zoneCount_ = 0;

// ---------------------------------------------------------------------------- parsing

String between(const String &src, const String &open, const String &close, int from = 0) {
  int a = src.indexOf(open, from);
  if (a < 0) return "";
  a += open.length();
  int b = src.indexOf(close, a);
  if (b < 0) return "";
  return src.substring(a, b);
}

String attr(const String &fragment, const String &name) {
  return between(fragment, name + "=\"", "\"");
}

// GetZoneGroupState returns XML escaped inside the SOAP body, sometimes twice.
String unescapeXml(String s) {
  for (int pass = 0; pass < 3 && s.indexOf("&lt;") >= 0; pass++) {
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&amp;", "&");
  }
  return s;
}

// XML-escape a value going INTO a SOAP body (line-in URIs are plain, but metadata and
// future URIs may contain & or <).
String escapeXml(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

// ------------------------------------------------------------------------------- SOAP

// Returns the response body, or "" on failure. NOTE: an empty body is ALSO a legitimate
// Sonos answer meaning "I am not the coordinator" — callers must not treat empty purely
// as an error.
String soapCall(const String &ip, const char *path, const char *service,
                const char *action, const String &extraParams = "",
                bool withInstanceId = true) {
  if (ip.isEmpty()) return "";

  String url = "http://" + ip + ":" + String(SONOS_PORT) + path;
  String body =
      "<?xml version=\"1.0\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:" + String(action) + " xmlns:u=\"" + service + "\">" +
      (withInstanceId ? "<InstanceID>0</InstanceID>" : "") + extraParams +
      "</u:" + String(action) + "></s:Body></s:Envelope>";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("[sonos] http.begin failed for %s\n", url.c_str());
    return "";
  }
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPAction", String("\"") + service + "#" + action + "\"");

  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : String("");
  if (code != 200) {
    Serial.printf("[sonos] %s -> HTTP %d\n", action, code);
    if (code > 0 && resp.length()) {
      String err = between(resp, "<errorCode>", "</errorCode>");
      if (err.length()) Serial.printf("[sonos] UPnP errorCode %s\n", err.c_str());
    }
    resp = "";
  }
  http.end();
  return resp;
}

// -------------------------------------------------------------------------- discovery

uint8_t discoverPlayers(String out[], uint8_t maxOut) {
  WiFiUDP udp;
  if (!udp.begin(0)) return 0;

  const char *msearch =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 3\r\n"
      "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
      "\r\n";

  IPAddress multicast(239, 255, 255, 250);
  udp.beginPacket(multicast, 1900);
  udp.write((const uint8_t *)msearch, strlen(msearch));
  udp.endPacket();

  uint8_t found = 0;
  uint32_t deadline = millis() + SSDP_WAIT_MS;
  while (millis() < deadline && found < maxOut) {
    if (udp.parsePacket() <= 0) { delay(10); continue; }
    String ip = udp.remoteIP().toString();
    while (udp.available()) udp.read();

    bool dup = false;
    for (uint8_t i = 0; i < found; i++) if (out[i] == ip) { dup = true; break; }
    if (!dup) out[found++] = ip;
  }
  udp.stop();
  return found;
}

// Fill zones_ from a GetZoneGroupState document.
void parseTopology(const String &topology) {
  zoneCount_ = 0;
  int pos = 0;

  while (zoneCount_ < MAX_ZONES) {
    int gStart = topology.indexOf("<ZoneGroup ", pos);
    if (gStart < 0) break;
    int gEnd = topology.indexOf("</ZoneGroup>", gStart);
    if (gEnd < 0) break;

    String group = topology.substring(gStart, gEnd);
    pos = gEnd + 1;

    String coordUuid = attr(group.substring(0, group.indexOf('>') + 1), "Coordinator");

    int mPos = 0;
    while (zoneCount_ < MAX_ZONES) {
      int mStart = group.indexOf("<ZoneGroupMember ", mPos);
      if (mStart < 0) break;
      int mEnd = group.indexOf('>', mStart);
      if (mEnd < 0) break;
      String member = group.substring(mStart, mEnd);
      mPos = mEnd + 1;

      String location = attr(member, "Location");
      String ip;
      int hostStart = location.indexOf("//");
      int portStart = location.indexOf(':', hostStart + 2);
      if (hostStart >= 0 && portStart > hostStart) {
        ip = location.substring(hostStart + 2, portStart);
      }

      Zone &z = zones_[zoneCount_++];
      z.room           = attr(member, "ZoneName");
      z.uuid           = attr(member, "UUID");
      z.ip             = ip;
      z.groupCoordUuid = coordUuid;
      z.invisible      = (attr(member, "Invisible") == "1");
    }
  }
}

const Zone *zoneByUuid(const String &uuid) {
  for (uint8_t i = 0; i < zoneCount_; i++) {
    if (zones_[i].uuid == uuid) return &zones_[i];
  }
  return nullptr;
}

// Remembered player IPs, so a reboot does not depend on SSDP.
//
// Multicast is genuinely unreliable on some home networks — on this one SSDP stopped
// answering entirely (from the board AND from a laptop) while every player was still
// reachable by direct HTTP. Without this the device cannot recover from a cold boot in
// that state, because the zone table starts empty and there is nothing to fall back to.
Preferences prefs_;

void saveKnownIps() {
  String joined;
  for (uint8_t i = 0; i < zoneCount_; i++) {
    if (zones_[i].ip.isEmpty()) continue;
    if (joined.length()) joined += ",";
    joined += zones_[i].ip;
  }
  if (joined.isEmpty()) return;
  if (prefs_.begin("sonos", false)) {
    prefs_.putString("ips", joined);
    prefs_.end();
  }
}

uint8_t loadKnownIps(String out[], uint8_t maxOut) {
  if (!prefs_.begin("sonos", true)) return 0;
  String joined = prefs_.getString("ips", "");
  prefs_.end();

  uint8_t n = 0;
  int start = 0;
  while (n < maxOut && start < (int)joined.length()) {
    int comma = joined.indexOf(',', start);
    if (comma < 0) comma = joined.length();
    String ip = joined.substring(start, comma);
    if (ip.length()) out[n++] = ip;
    start = comma + 1;
  }
  return n;
}

// Ask one specific player for the topology and rebuild the zone table from it.
bool topologyFrom(const String &ip) {
  String resp = soapCall(ip, PATH_TOPOLOGY, SVC_TOPOLOGY, "GetZoneGroupState");
  if (resp.isEmpty()) return false;

  String state = unescapeXml(between(resp, "<ZoneGroupState>", "</ZoneGroupState>"));
  if (state.isEmpty()) state = unescapeXml(resp);
  if (state.indexOf("<ZoneGroup ") < 0) return false;

  parseTopology(state);
  if (zoneCount_ > 0) saveKnownIps();   // survive a reboot without SSDP
  return zoneCount_ > 0;
}

// Recompute the cached coordinator for the configured target room.
//
// If the target room is missing (its speaker is powered off), fall back to ANY available
// group rather than giving up: one offline speaker must not disable the volume knob for
// the speakers that are still playing.
bool applyCoordinator() {
  targetMissing_ = false;

  const Zone *coord = coordinatorForRoom(targetRoom_.isEmpty() ? nullptr
                                                               : targetRoom_.c_str());
  if (!coord && !targetRoom_.isEmpty()) {
    coord = coordinatorForRoom(nullptr);      // first group that exists
    if (coord) {
      targetMissing_ = true;
      Serial.printf("[sonos] room \"%s\" not present — falling back to \"%s\"\n",
                    targetRoom_.c_str(), coord->room.c_str());
    }
  }
  if (!coord) return false;

  coordinatorIp_   = coord->ip;
  coordinatorRoom_ = coord->room;
  return true;
}

}  // namespace

// -------------------------------------------------------------------------- public API

void begin(const char *preferredIp, const char *targetRoom) {
  preferredIp_ = preferredIp ? preferredIp : "";
  targetRoom_  = targetRoom ? targetRoom : "";
  coordinatorIp_ = "";
  coordinatorRoom_ = "";
  zoneCount_ = 0;
}

ResolveResult resolveCoordinator() {
  coordinatorIp_ = "";
  coordinatorRoom_ = "";

  String candidates[MAX_ZONES];
  uint8_t count = 0;

  auto addCandidate = [&](const String &ip) {
    if (ip.isEmpty() || count >= MAX_ZONES) return;
    for (uint8_t i = 0; i < count; i++) if (candidates[i] == ip) return;
    candidates[count++] = ip;
  };

  // Order matters: cheapest and most likely first. SSDP is LAST because it is the one
  // step that fails wholesale when multicast is filtered.
  addCandidate(preferredIp_);
  addCandidate(lastGoodIp_);
  for (uint8_t i = 0; i < zoneCount_; i++) addCandidate(zones_[i].ip);

  String saved[MAX_ZONES];
  uint8_t s = loadKnownIps(saved, MAX_ZONES);
  for (uint8_t i = 0; i < s; i++) addCandidate(saved[i]);
  if (s) Serial.printf("[sonos] %u remembered player IP(s) from NVS\n", s);

  String discovered[MAX_ZONES];
  uint8_t n = discoverPlayers(discovered, MAX_ZONES);
  for (uint8_t i = 0; i < n; i++) addCandidate(discovered[i]);

  if (count == 0) return ResolveResult::NoPlayersFound;

  // SSDP can surface players from MULTIPLE Sonos households (seen live). Only members of
  // a household's own topology are usable, so trust the document a candidate returns.
  bool sawTopology = false;
  for (uint8_t i = 0; i < count; i++) {
    if (!topologyFrom(candidates[i])) continue;
    sawTopology = true;
    lastGoodIp_ = candidates[i];
    if (applyCoordinator()) {
      Serial.printf("[sonos] %u zones; coordinator %s (\"%s\")\n",
                    zoneCount_, coordinatorIp_.c_str(), coordinatorRoom_.c_str());
      return ResolveResult::Ok;
    }
  }

  if (!sawTopology) return ResolveResult::NoTopology;
  return ResolveResult::RoomNotFound;
}

bool refreshTopology() {
  // Fast path: one POST to a player we already know.
  if (!lastGoodIp_.isEmpty() && topologyFrom(lastGoodIp_) && applyCoordinator()) {
    return true;
  }

  // The cached player may simply be switched off while the rest of the system is fine
  // (seen live: the Era 300 went offline but both Era 100s kept playing). Ask every OTHER
  // zone we already know about before paying for a full SSDP sweep — any of them can serve
  // the whole topology.
  for (uint8_t i = 0; i < zoneCount_; i++) {
    const String &ip = zones_[i].ip;
    if (ip.isEmpty() || ip == lastGoodIp_) continue;
    if (topologyFrom(ip) && applyCoordinator()) {
      lastGoodIp_ = ip;
      Serial.printf("[sonos] topology recovered via %s\n", ip.c_str());
      return true;
    }
  }

  Serial.println("[sonos] no known player answered, falling back to discovery");
  return resolveCoordinator() == ResolveResult::Ok;
}

uint8_t zoneCount() { return zoneCount_; }

const Zone *zoneAt(uint8_t i) { return (i < zoneCount_) ? &zones_[i] : nullptr; }

const Zone *zoneByRoom(const char *room) {
  if (!room) return nullptr;
  for (uint8_t i = 0; i < zoneCount_; i++) {
    // Skip bonded satellites: they answer nothing useful and must never be targeted.
    if (!zones_[i].invisible && zones_[i].room.equalsIgnoreCase(room)) return &zones_[i];
  }
  return nullptr;
}

const Zone *coordinatorForRoom(const char *room) {
  if (room == nullptr) {
    // No room requested: fall back to the first group's coordinator.
    for (uint8_t i = 0; i < zoneCount_; i++) {
      if (zones_[i].invisible) continue;
      const Zone *c = zoneByUuid(zones_[i].groupCoordUuid);
      if (c) return c;
    }
    return nullptr;
  }
  const Zone *z = zoneByRoom(room);
  if (!z) return nullptr;
  return zoneByUuid(z->groupCoordUuid);
}

bool roomsGrouped(const char *roomA, const char *roomB) {
  const Zone *a = zoneByRoom(roomA);
  const Zone *b = zoneByRoom(roomB);
  if (!a || !b) return false;
  return a->groupCoordUuid == b->groupCoordUuid;
}

// ------------------------------------------------------------------------------ volume

bool getGroupVolume(int &volumeOut) {
  String resp = soapCall(coordinatorIp_, PATH_GROUP_RENDERING, SVC_GROUP_RENDERING,
                         "GetGroupVolume");
  if (resp.isEmpty()) return false;
  String v = between(resp, "<CurrentVolume>", "</CurrentVolume>");
  if (v.isEmpty()) return false;
  volumeOut = v.toInt();
  return true;
}

bool setRelativeGroupVolume(int adjustment, int &newVolumeOut) {
  String params = "<Adjustment>" + String(adjustment) + "</Adjustment>";
  String resp = soapCall(coordinatorIp_, PATH_GROUP_RENDERING, SVC_GROUP_RENDERING,
                         "SetRelativeGroupVolume", params);
  if (resp.isEmpty()) return false;
  String v = between(resp, "<NewVolume>", "</NewVolume>");
  if (v.isEmpty()) return false;
  newVolumeOut = v.toInt();
  return true;
}

bool getRoomVolume(const char *room, int &volumeOut) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  // Channel=Master is required by RenderingControl; GroupRenderingControl does not take it.
  String resp = soapCall(z->ip, PATH_RENDERING, SVC_RENDERING, "GetVolume",
                         "<Channel>Master</Channel>");
  if (resp.isEmpty()) return false;
  String v = between(resp, "<CurrentVolume>", "</CurrentVolume>");
  if (v.isEmpty()) return false;
  volumeOut = v.toInt();
  return true;
}

bool setRelativeRoomVolume(const char *room, int adjustment, int &newVolumeOut) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  String params = "<Channel>Master</Channel><Adjustment>" + String(adjustment) +
                  "</Adjustment>";
  String resp = soapCall(z->ip, PATH_RENDERING, SVC_RENDERING, "SetRelativeVolume", params);
  if (resp.isEmpty()) return false;
  String v = between(resp, "<NewVolume>", "</NewVolume>");
  if (v.isEmpty()) return false;
  newVolumeOut = v.toInt();
  return true;
}

bool setRoomVolume(const char *room, int volume) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  // A bonded stereo pair is one zone: setting the visible member sets both speakers.
  String params = "<Channel>Master</Channel><DesiredVolume>" + String(volume) +
                  "</DesiredVolume>";
  return !soapCall(z->ip, PATH_RENDERING, SVC_RENDERING, "SetVolume", params).isEmpty();
}

uint8_t adjustVolumeAllRooms(int adjustment, int &volumeOut) {
  uint8_t adjusted = 0;
  int sum = 0;

  for (uint8_t i = 0; i < zoneCount_; i++) {
    const Zone &z = zones_[i];
    // Bonded satellites follow their pair coordinator: adjusting them separately would
    // double-apply and break the stereo pair's balance.
    if (z.invisible) continue;

    // One call per ROOM. Guard against a duplicate visible entry for the same room.
    bool already = false;
    for (uint8_t j = 0; j < i; j++) {
      if (!zones_[j].invisible && zones_[j].room.equalsIgnoreCase(zones_[i].room)) {
        already = true;
        break;
      }
    }
    if (already) continue;

    String params = "<Channel>Master</Channel><Adjustment>" + String(adjustment) +
                    "</Adjustment>";
    String resp = soapCall(z.ip, PATH_RENDERING, SVC_RENDERING, "SetRelativeVolume", params);
    if (resp.isEmpty()) continue;

    String v = between(resp, "<NewVolume>", "</NewVolume>");
    if (v.isEmpty()) continue;
    sum += v.toInt();
    adjusted++;
  }

  if (adjusted) volumeOut = sum / adjusted;
  return adjusted;
}

// ---------------------------------------------------------------------------- grouping

bool joinRoomTo(const char *room, const char *targetRoom) {
  const Zone *z = zoneByRoom(room);
  const Zone *target = coordinatorForRoom(targetRoom);
  if (!z || !target) {
    Serial.printf("[sonos] joinRoomTo: unknown room (%s -> %s)\n",
                  room ? room : "?", targetRoom ? targetRoom : "?");
    return false;
  }
  if (z->uuid == target->uuid) return true;   // already the coordinator of that group

  String params = "<CurrentURI>x-rincon:" + target->uuid + "</CurrentURI>"
                  "<CurrentURIMetaData></CurrentURIMetaData>";
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT,
                         "SetAVTransportURI", params);
  return !resp.isEmpty();
}

bool detachRoom(const char *room) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;

  bool isCoordinator = (z->uuid == z->groupCoordUuid);

  if (isCoordinator) {
    // Find another VISIBLE member of the same group to hand coordination to. Without this
    // the stream dies when the coordinator leaves.
    const Zone *successor = nullptr;
    for (uint8_t i = 0; i < zoneCount_; i++) {
      if (zones_[i].invisible) continue;
      if (zones_[i].uuid == z->uuid) continue;
      if (zones_[i].groupCoordUuid == z->groupCoordUuid) { successor = &zones_[i]; break; }
    }

    if (successor) {
      String params = "<NewCoordinator>" + successor->uuid + "</NewCoordinator>"
                      "<RejoinGroup>0</RejoinGroup>";
      String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT,
                             "DelegateGroupCoordinationTo", params);
      if (resp.isEmpty()) {
        Serial.println("[sonos] DelegateGroupCoordinationTo failed");
        return false;
      }
      Serial.printf("[sonos] handed coordination of \"%s\" to \"%s\"\n",
                    z->room.c_str(), successor->room.c_str());
      return true;
    }
    // Sole member: it is already its own standalone group, nothing to detach.
    return true;
  }

  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT,
                         "BecomeCoordinatorOfStandaloneGroup");
  return !resp.isEmpty();
}

// ----------------------------------------------------------------------------- line-in

bool playLineIn(const char *room) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;

  // A player's own analogue input is addressed by its OWN uuid.
  String uri = "x-rincon-stream:" + z->uuid;
  String params = "<CurrentURI>" + escapeXml(uri) + "</CurrentURI>"
                  "<CurrentURIMetaData></CurrentURIMetaData>";
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT,
                         "SetAVTransportURI", params);
  if (resp.isEmpty()) return false;

  resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "Play", "<Speed>1</Speed>");
  return !resp.isEmpty();
}

bool isLineInActive(const char *room, bool &activeOut) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetMediaInfo");
  if (resp.isEmpty()) return false;
  String uri = between(resp, "<CurrentURI>", "</CurrentURI>");
  activeOut = uri.startsWith("x-rincon-stream:");
  return true;
}

bool isPlaying(const char *room, bool &playingOut) {
  const Zone *z = coordinatorForRoom(room);
  if (!z) return false;
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetTransportInfo");
  if (resp.isEmpty()) return false;
  String state = between(resp, "<CurrentTransportState>", "</CurrentTransportState>");
  playingOut = (state == "PLAYING" || state == "TRANSITIONING");
  return true;
}

const char *coordinatorIp()   { return coordinatorIp_.c_str(); }
const char *coordinatorRoom() { return coordinatorRoom_.c_str(); }
bool haveCoordinator()        { return !coordinatorIp_.isEmpty(); }
bool targetRoomMissing()      { return targetMissing_; }

const char *resolveResultText(ResolveResult r) {
  switch (r) {
    case ResolveResult::Ok:             return "OK";
    case ResolveResult::NoPlayersFound: return "no ZonePlayer answered SSDP";
    case ResolveResult::NoTopology:     return "players found but no topology";
    case ResolveResult::RoomNotFound:   return "target room not in any group";
  }
  return "?";
}

}  // namespace sonos
