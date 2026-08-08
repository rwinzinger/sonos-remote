#include "sonos.h"

#include <HTTPClient.h>
#include <ESPmDNS.h>
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
const char *PATH_DEVICEPROPS     = "/DeviceProperties/Control";
const char *SVC_GROUP_RENDERING  = "urn:schemas-upnp-org:service:GroupRenderingControl:1";
const char *SVC_AVTRANSPORT      = "urn:schemas-upnp-org:service:AVTransport:1";
const char *SVC_RENDERING        = "urn:schemas-upnp-org:service:RenderingControl:1";
const char *SVC_TOPOLOGY         = "urn:schemas-upnp-org:service:ZoneGroupTopology:1";
const char *SVC_DEVICEPROPS      = "urn:schemas-upnp-org:service:DeviceProperties:1";

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

// "UUID_LF:LF,LF;UUID_RF:RF,RF" — the argument both SeparateStereoPair and CreateStereoPair
// need. Only visible in the topology while the pair EXISTS, so it is cached in NVS.
String pairMapSet_;

// The room name that MEANS "the stereo pair". Needed because splitting the pair makes
// Sonos revert both halves to their pre-pair names (seen live: "Stereo" became
// "Wohnzimmer"), and re-pairing KEEPS the reverted name — so a name lookup for the pair
// fails permanently after the first TV mode. Names are user-editable and Sonos-editable;
// the pair's UUID is not.
String stereoRoomName_;

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

// Sonos players advertise _sonos._tcp over mDNS — verified live on this network, with
// instance names of the form "RINCON_<uuid>@<roomName>". This is the discovery path the
// Sonos app effectively uses, and it works where SSDP multicast does not, which is why it
// is tried BEFORE SSDP here.
bool mdnsStarted_ = false;

uint8_t discoverViaMdns(String out[], uint8_t maxOut) {
  if (!mdnsStarted_) {
    // The hostname we register is irrelevant to querying; it just has to be unique-ish.
    mdnsStarted_ = MDNS.begin("sonos-remote");
    if (!mdnsStarted_) {
      Serial.println("[sonos] MDNS.begin failed");
      return 0;
    }
  }

  int n = MDNS.queryService("sonos", "tcp");
  uint8_t found = 0;
  for (int i = 0; i < n && found < maxOut; i++) {
    String ip = MDNS.IP(i).toString();
    if (ip.isEmpty() || ip == "0.0.0.0") continue;
    bool dup = false;
    for (uint8_t j = 0; j < found; j++) if (out[j] == ip) { dup = true; break; }
    if (!dup) out[found++] = ip;
  }
  if (found) Serial.printf("[sonos] mDNS found %u player(s)\n", found);
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

      // Seen on both members of a bonded pair; remember it so the pair can be rebuilt.
      String cms = attr(member, "ChannelMapSet");
      if (cms.length() && cms.indexOf(":RF") >= 0) pairMapSet_ = cms;
    }
  }
}

const Zone *zoneByUuid(const String &uuid) {
  for (uint8_t i = 0; i < zoneCount_; i++) {
    if (zones_[i].uuid == uuid) return &zones_[i];
  }
  return nullptr;
}

// "UUID_LF:LF,LF;UUID_RF:RF,RF" -> the UUID marked RF / LF.
String rfUuidFromMapSet(const String &cms) {
  int rf = cms.indexOf(":RF");
  if (rf < 0) return "";
  int start = cms.lastIndexOf(';', rf);
  start = (start < 0) ? 0 : start + 1;
  return cms.substring(start, rf);
}

String lfUuidFromMapSet(const String &cms) {
  int lf = cms.indexOf(":LF");
  if (lf < 0) return "";
  int start = cms.lastIndexOf(';', lf);
  start = (start < 0) ? 0 : start + 1;
  return cms.substring(start, lf);
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
  if (joined.isEmpty() && pairMapSet_.isEmpty()) return;
  if (prefs_.begin("sonos", false)) {
    if (joined.length())      prefs_.putString("ips", joined);
    if (pairMapSet_.length()) prefs_.putString("cms", pairMapSet_);
    prefs_.end();
  }
}

uint8_t loadKnownIps(String out[], uint8_t maxOut) {
  if (!prefs_.begin("sonos", true)) return 0;
  String joined = prefs_.getString("ips", "");
  if (pairMapSet_.isEmpty()) pairMapSet_ = prefs_.getString("cms", "");
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

void setStereoRoomName(const char *room) {
  stereoRoomName_ = room ? room : "";
}

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

  // mDNS before SSDP: SSDP multicast stopped answering entirely on this network once,
  // from the board AND from a laptop, while mDNS kept working.
  String viaMdns[MAX_ZONES];
  uint8_t m = discoverViaMdns(viaMdns, MAX_ZONES);
  for (uint8_t i = 0; i < m; i++) addCandidate(viaMdns[i]);

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

  // Fallback for the stereo pair ONLY. Separating the pair makes Sonos revert both halves
  // to their pre-pair names, and re-pairing keeps the reverted name — so after one TV mode
  // the configured name matches nothing and every lookup for it fails permanently. The
  // pair's LF UUID is stable across split/rebuild, so use that instead.
  if (!stereoRoomName_.isEmpty() && !pairMapSet_.isEmpty() &&
      stereoRoomName_.equalsIgnoreCase(room)) {
    const Zone *lf = zoneByUuid(lfUuidFromMapSet(pairMapSet_));
    if (lf && !lf->invisible) return lf;
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

    // NOTE: do NOT de-duplicate by room NAME. Separating a stereo pair leaves both halves
    // carrying the same name (seen live: both became "Wohnzimmer"), and skipping the second
    // one means the right speaker's volume silently never moves. Bonded satellites are
    // already excluded by the invisible check above, which is the only case that needed it.

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

// ------------------------------------------------------------------------ stereo pair

bool stereoPairKnown() { return !pairMapSet_.isEmpty(); }

bool stereoPairSeparated() {
  if (pairMapSet_.isEmpty()) return false;
  String rf = rfUuidFromMapSet(pairMapSet_);
  if (rf.isEmpty()) return false;
  // While paired the RF speaker is Invisible; once separated it stands as its own zone.
  const Zone *z = zoneByUuid(rf);
  return z && !z->invisible;
}

const char *rightSpeakerRoom() {
  static String room;
  room = "";
  if (pairMapSet_.isEmpty()) return room.c_str();
  const Zone *z = zoneByUuid(rfUuidFromMapSet(pairMapSet_));
  if (z && !z->invisible) room = z->room;
  return room.c_str();
}

bool separateStereoPair() {
  if (pairMapSet_.isEmpty()) {
    Serial.println("[sonos] no ChannelMapSet known — cannot separate the pair");
    return false;
  }
  // Send to the LF speaker: it is the pair's coordinator and the one that still answers.
  const Zone *lf = zoneByUuid(lfUuidFromMapSet(pairMapSet_));
  if (!lf) return false;
  String params = "<ChannelMapSet>" + escapeXml(pairMapSet_) + "</ChannelMapSet>";
  // DeviceProperties actions take NO InstanceID.
  String resp = soapCall(lf->ip, PATH_DEVICEPROPS, SVC_DEVICEPROPS, "SeparateStereoPair",
                         params, false);
  Serial.printf("[sonos] SeparateStereoPair: %s\n", resp.isEmpty() ? "FAILED" : "ok");
  return !resp.isEmpty();
}

// Restore the pair's zone name. Rebuilding a pair leaves it under the PRE-PAIR name
// ("Wohnzimmer" here), so without this every TV toggle silently renames the user's room.
// Icon and configuration are read first and passed back unchanged — SetZoneAttributes
// takes all three, and omitting them would blank them.
bool renameStereoPair(const char *name) {
  if (pairMapSet_.isEmpty() || !name || !*name) return false;
  const Zone *lf = zoneByUuid(lfUuidFromMapSet(pairMapSet_));
  if (!lf) return false;

  String cur = soapCall(lf->ip, PATH_DEVICEPROPS, SVC_DEVICEPROPS, "GetZoneAttributes",
                        "", false);
  if (cur.isEmpty()) return false;
  String curName = between(cur, "<CurrentZoneName>", "</CurrentZoneName>");
  if (curName == name) return true;                    // already correct, no write

  String icon = between(cur, "<CurrentIcon>", "</CurrentIcon>");
  String conf = between(cur, "<CurrentConfiguration>", "</CurrentConfiguration>");
  String params = "<DesiredZoneName>" + escapeXml(String(name)) + "</DesiredZoneName>"
                  "<DesiredIcon>" + escapeXml(icon) + "</DesiredIcon>"
                  "<DesiredConfiguration>" + escapeXml(conf) + "</DesiredConfiguration>";
  bool ok = !soapCall(lf->ip, PATH_DEVICEPROPS, SVC_DEVICEPROPS, "SetZoneAttributes",
                      params, false).isEmpty();
  Serial.printf("[sonos] renamed pair \"%s\" -> \"%s\": %s\n",
                curName.c_str(), name, ok ? "ok" : "FAILED");
  return ok;
}

bool createStereoPair() {
  if (pairMapSet_.isEmpty()) return false;
  const Zone *lf = zoneByUuid(lfUuidFromMapSet(pairMapSet_));
  if (!lf) return false;
  String params = "<ChannelMapSet>" + escapeXml(pairMapSet_) + "</ChannelMapSet>";
  String resp = soapCall(lf->ip, PATH_DEVICEPROPS, SVC_DEVICEPROPS, "CreateStereoPair",
                         params, false);
  Serial.printf("[sonos] CreateStereoPair: %s\n", resp.isEmpty() ? "FAILED" : "ok");
  if (resp.isEmpty()) return false;

  // Done HERE rather than at the call sites so it cannot be forgotten by a future scene.
  // The speaker needs a moment after re-pairing before it accepts the rename.
  if (!stereoRoomName_.isEmpty()) {
    delay(2000);
    refreshTopology();                       // the rebuilt pair needs re-locating first
    renameStereoPair(stereoRoomName_.c_str());
  }
  return true;
}

// Stop every visible zone except the one with this UUID. Used by TV mode: after a split,
// room NAMES are unreliable (both halves are called the same thing), so target by UUID.
bool stopAllExcept(const char *keepUuid) {
  bool allOk = true;
  for (uint8_t i = 0; i < zoneCount_; i++) {
    if (zones_[i].invisible) continue;
    if (keepUuid && zones_[i].uuid == keepUuid) continue;
    if (soapCall(zones_[i].ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "Stop", "<Speed>1</Speed>")
            .isEmpty()) {
      allOk = false;
    }
  }
  return allOk;
}

const char *rightSpeakerUuid() {
  static String uuid;
  uuid = pairMapSet_.isEmpty() ? String("") : rfUuidFromMapSet(pairMapSet_);
  return uuid.c_str();
}

bool stopRoom(const char *room) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  return !soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "Stop", "<Speed>1</Speed>")
              .isEmpty();
}

bool playUriOn(const char *room, const char *uri) {
  const Zone *z = zoneByRoom(room);
  if (!z || !uri || !*uri) return false;
  String params = "<CurrentURI>" + escapeXml(String(uri)) + "</CurrentURI>"
                  "<CurrentURIMetaData></CurrentURIMetaData>";
  if (soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "SetAVTransportURI",
               params).isEmpty()) {
    return false;
  }
  return !soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "Play", "<Speed>1</Speed>")
              .isEmpty();
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

// Is this room playing a Bluetooth / virtual line-in source? Sonos exposes those as
// "x-sonos-vli:<uuid>:<n>,bluetooth:<n>" — seen live while the TV was connected.
bool isBluetoothActive(const char *room, bool &activeOut) {
  const Zone *z = zoneByRoom(room);
  if (!z) return false;
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetMediaInfo");
  if (resp.isEmpty()) return false;
  String uri = between(resp, "<CurrentURI>", "</CurrentURI>");
  activeOut = (uri.indexOf("bluetooth") >= 0);
  return true;
}

// Radio streams often carry the stream URL in dc:title until Sonos resolves the station,
// and TrackURI/<res> always hold it. Anything URI-shaped is unusable as a display name.
static bool looksLikeUri(const String &v) {
  return v.indexOf("://") >= 0 || v.startsWith("x-");
}

// Radio stations rotate r:streamContent between the actual track and promo strings like
// "www.radioeins.de", so the display flaps between them. A bare host has NO SPACES and a
// short alphabetic suffix after a dot; real track text almost always contains a space.
// Rejecting these makes the line settle on the station name instead of cycling.
static bool looksLikePromo(const String &v) {
  if (v.indexOf(' ') >= 0) return false;          // has a space: treat as real text
  if (v.indexOf("www.") >= 0) return true;
  int dot = v.lastIndexOf('.');
  if (dot <= 0 || dot >= (int)v.length() - 1) return false;
  String tld = v.substring(dot + 1);
  if (tld.length() < 2 || tld.length() > 4) return false;
  for (unsigned i = 0; i < tld.length(); i++) {
    if (!isAlpha(tld[i])) return false;
  }
  return true;                                     // e.g. "radioeins.de"
}

static bool usableText(const String &v) {
  return v.length() && !looksLikeUri(v) && !looksLikePromo(v);
}

bool getNowPlaying(const char *room, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';

  const Zone *z = coordinatorForRoom(room);
  if (!z) return false;

  String info = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetTransportInfo");
  String state = between(info, "<CurrentTransportState>", "</CurrentTransportState>");
  if (state != "PLAYING" && state != "TRANSITIONING") return false;

  String title, artist;
  String pos = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetPositionInfo");
  if (!pos.isEmpty()) {
    String meta = unescapeXml(between(pos, "<TrackMetaData>", "</TrackMetaData>"));
    artist = between(meta, "<dc:creator>", "</dc:creator>");

    // Preference order matters. streamContent carries what a station is playing RIGHT NOW,
    // which beats the station's own name; dc:title is next; and either may legitimately be
    // absent. Anything URI-shaped is discarded rather than shown.
    String stream = between(meta, "<r:streamContent>", "</r:streamContent>");
    String dcTitle = between(meta, "<dc:title>", "</dc:title>");
    if (usableText(stream))       title = stream;
    else if (usableText(dcTitle))  title = dcTitle;
    if (!usableText(artist)) artist = "";
  }

  if (title.isEmpty()) {
    // No track info: name the SOURCE instead of showing nothing.
    String mi  = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetMediaInfo");
    String uri = between(mi, "<CurrentURI>", "</CurrentURI>");
    String umeta = unescapeXml(between(mi, "<CurrentURIMetaData>", "</CurrentURIMetaData>"));
    String label = between(umeta, "<dc:title>", "</dc:title>");

    if (uri.startsWith("x-rincon-stream:"))            title = "Plattenspieler";
    else if (uri.indexOf("bluetooth") >= 0)            title = "Bluetooth";
    else if (usableText(label))                        title = label;
    else if (uri.startsWith("x-rincon-mp3radio"))      title = "Radio";
    else if (uri.startsWith("x-sonos-vli"))            title = "Line-in";
  }
  if (title.isEmpty()) return false;

  String line = title;
  // ASCII separator on purpose: LVGL's Montserrat build carries ASCII plus a symbol range,
  // so U+00B7 (middle dot) and en/em dashes render as placeholder boxes.
  if (artist.length() && !artist.equalsIgnoreCase(title)) line += " - " + artist;
  strncpy(out, line.c_str(), outLen - 1);
  out[outLen - 1] = '\0';
  return true;
}

bool isPlayingUuid(const char *uuid, bool &playingOut) {
  if (!uuid || !*uuid) return false;
  const Zone *z = zoneByUuid(String(uuid));
  if (!z) return false;
  String resp = soapCall(z->ip, PATH_AVTRANSPORT, SVC_AVTRANSPORT, "GetTransportInfo");
  if (resp.isEmpty()) return false;
  String state = between(resp, "<CurrentTransportState>", "</CurrentTransportState>");
  playingOut = (state == "PLAYING" || state == "TRANSITIONING");
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
