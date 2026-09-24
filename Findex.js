const CORS_HEADERS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, DELETE, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type',
};

const MAX_QUEUE_DEPTH = 12;
const MAX_TS = 99999999999999; // 14 nines — used to flip timestamp ordering for "most recent first" event listing

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { ...CORS_HEADERS, 'Content-Type': 'application/json' },
  });
}

// every queued item for a lamp lives at queue/{lampId}/{itemId}.png, where
// itemId starts with a millisecond timestamp — so a plain lexicographic
// sort on the R2 key doubles as chronological order, oldest first
async function getQueueItems(env, lampId) {
  const listing = await env.LAMP_IMAGES.list({ prefix: `queue/${lampId}/` });
  return listing.objects.slice().sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
}

// same lexicographic-sort-doubles-as-chronological trick, for the public
// image gallery's gallery-images/{galleryId}/{itemId} keys
async function getGalleryImageItems(env, galleryId) {
  const listing = await env.LAMP_IMAGES.list({ prefix: `gallery-images/${galleryId}/` });
  return listing.objects.slice().sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
}
function extFromContentType(ct) {
  if (!ct) return 'png';
  if (ct.includes('jpeg') || ct.includes('jpg')) return 'jpg';
  if (ct.includes('webp')) return 'webp';
  if (ct.includes('gif')) return 'gif';
  return 'png';
}
function contentTypeFromExt(ext) {
  if (ext === 'jpg' || ext === 'jpeg') return 'image/jpeg';
  if (ext === 'webp') return 'image/webp';
  if (ext === 'gif') return 'image/gif';
  return 'image/png';
}

// pulls Cloudflare's built-in edge geolocation off any request — no extra
// API call, this data rides along with every request Cloudflare proxies
function geoFromRequest(request) {
  const cf = request.cf || {};
  return {
    lat: cf.latitude ? parseFloat(cf.latitude) : null,
    lon: cf.longitude ? parseFloat(cf.longitude) : null,
    city: cf.city || null,
    region: cf.region || null,
    postalCode: cf.postalCode || null,
    country: cf.country || null,
    timezone: cf.timezone || null,
    colo: cf.colo || null, // which Cloudflare edge datacenter served this — not the visitor's location, but useful context
  };
}

// ============================================================
// EVENT LOG: one permanent record per delivered item, independent of the
// queue image itself (which gets deleted on ack — this doesn't). Built for
// the eventual Gallery/interaction-map page.
//
// Two KV keys per event, written at upload time:
//   event-index:{lampId}:{itemId}  -> the real event key (cheap O(1) lookup
//                                     at download time, since itemId is all
//                                     the download-side code has on hand)
//   event:{reverseTs}:{lampId}:{itemId} -> the actual JSON record
// reverseTs = MAX_TS - sentAt, zero-padded, so a plain ascending KV list on
// the "event:" prefix naturally comes back MOST RECENT FIRST — no need to
// fetch everything and sort client-side for a live feed.
// ============================================================
function eventIndexKey(lampId, itemId) { return `event-index:${lampId}:${itemId}`; }

async function recordSentEvent(env, lampId, itemId, request, sentAt) {
  const reverseTs = String(MAX_TS - sentAt).padStart(14, '0');
  const eventKey = `event:${reverseTs}:${lampId}:${itemId}`;
  const record = {
    lampId, itemId,
    sentAt, sentGeo: geoFromRequest(request),
    downloadedAt: null, downloadGeo: null,
    deliveryMs: null,
  };
  await env.LAMP_KV.put(eventKey, JSON.stringify(record));
  await env.LAMP_KV.put(eventIndexKey(lampId, itemId), eventKey);
}

async function recordDownloadEvent(env, lampId, itemId, request) {
  const eventKey = await env.LAMP_KV.get(eventIndexKey(lampId, itemId));
  if (!eventKey) return; // sent-event missing (shouldn't happen) — skip rather than throw
  const raw = await env.LAMP_KV.get(eventKey);
  if (!raw) return;
  const record = JSON.parse(raw);
  if (record.downloadedAt) return; // keep the FIRST download, in case of a retry/re-pull before ack
  record.downloadedAt = Date.now();
  record.downloadGeo = geoFromRequest(request);
  record.deliveryMs = record.downloadedAt - record.sentAt;
  await env.LAMP_KV.put(eventKey, JSON.stringify(record));
}

// ============================================================
// DEV/TESTING ONLY: real request.cf geo can't be spoofed on a genuine
// upload/download call — it's populated by Cloudflare from the actual
// requester's IP. Rather than fight that, /gallery/seed writes fake event
// records directly, in the exact same shape real ones end up in, using
// these real-world city coordinates. No auth on this yet, same as
// everything else in this Worker right now — fine for now, but this route
// (and the rest) should get some form of access control before this is a
// public-facing product.
// ============================================================
const CITY_PRESETS = {
  'New York': { lat: 40.7128, lon: -74.0060, city: 'New York', region: 'New York', country: 'US', postalCode: '10001', timezone: 'America/New_York' },
  'Los Angeles': { lat: 34.0522, lon: -118.2437, city: 'Los Angeles', region: 'California', country: 'US', postalCode: '90001', timezone: 'America/Los_Angeles' },
  'London': { lat: 51.5072, lon: -0.1276, city: 'London', region: 'England', country: 'GB', postalCode: 'EC1A', timezone: 'Europe/London' },
  'Paris': { lat: 48.8566, lon: 2.3522, city: 'Paris', region: 'Ile-de-France', country: 'FR', postalCode: '75001', timezone: 'Europe/Paris' },
  'Berlin': { lat: 52.5200, lon: 13.4050, city: 'Berlin', region: 'Berlin', country: 'DE', postalCode: '10115', timezone: 'Europe/Berlin' },
  'Tokyo': { lat: 35.6762, lon: 139.6503, city: 'Tokyo', region: 'Tokyo', country: 'JP', postalCode: '100-0001', timezone: 'Asia/Tokyo' },
  'Seoul': { lat: 37.5665, lon: 126.9780, city: 'Seoul', region: 'Seoul', country: 'KR', postalCode: '03000', timezone: 'Asia/Seoul' },
  'Singapore': { lat: 1.3521, lon: 103.8198, city: 'Singapore', region: 'Singapore', country: 'SG', postalCode: '018956', timezone: 'Asia/Singapore' },
  'Mumbai': { lat: 19.0760, lon: 72.8777, city: 'Mumbai', region: 'Maharashtra', country: 'IN', postalCode: '400001', timezone: 'Asia/Kolkata' },
  'Sydney': { lat: -33.8688, lon: 151.2093, city: 'Sydney', region: 'New South Wales', country: 'AU', postalCode: '2000', timezone: 'Australia/Sydney' },
  'Sao Paulo': { lat: -23.5505, lon: -46.6333, city: 'Sao Paulo', region: 'Sao Paulo', country: 'BR', postalCode: '01000-000', timezone: 'America/Sao_Paulo' },
  'Mexico City': { lat: 19.4326, lon: -99.1332, city: 'Mexico City', region: 'CDMX', country: 'MX', postalCode: '01000', timezone: 'America/Mexico_City' },
  'Cairo': { lat: 30.0444, lon: 31.2357, city: 'Cairo', region: 'Cairo', country: 'EG', postalCode: '11511', timezone: 'Africa/Cairo' },
  'Cape Town': { lat: -33.9249, lon: 18.4241, city: 'Cape Town', region: 'Western Cape', country: 'ZA', postalCode: '8000', timezone: 'Africa/Johannesburg' },
  'Toronto': { lat: 43.6532, lon: -79.3832, city: 'Toronto', region: 'Ontario', country: 'CA', postalCode: 'M5H', timezone: 'America/Toronto' },
  "Chicago": { lat: 41.8781, lon: -87.6298, city: "Chicago", region: "Illinois", country: "US", postalCode: "60601", timezone: "America/Chicago" },
  "Houston": { lat: 29.7604, lon: -95.3698, city: "Houston", region: "Texas", country: "US", postalCode: "77001", timezone: "America/Chicago" },
  "San Francisco": { lat: 37.7749, lon: -122.4194, city: "San Francisco", region: "California", country: "US", postalCode: "94102", timezone: "America/Los_Angeles" },
  "Miami": { lat: 25.7617, lon: -80.1918, city: "Miami", region: "Florida", country: "US", postalCode: "33101", timezone: "America/New_York" },
  "Seattle": { lat: 47.6062, lon: -122.3321, city: "Seattle", region: "Washington", country: "US", postalCode: "98101", timezone: "America/Los_Angeles" },
  "Boston": { lat: 42.3601, lon: -71.0589, city: "Boston", region: "Massachusetts", country: "US", postalCode: "02108", timezone: "America/New_York" },
  "Denver": { lat: 39.7392, lon: -104.9903, city: "Denver", region: "Colorado", country: "US", postalCode: "80201", timezone: "America/Denver" },
  "Atlanta": { lat: 33.749, lon: -84.388, city: "Atlanta", region: "Georgia", country: "US", postalCode: "30301", timezone: "America/New_York" },
  "Dallas": { lat: 32.7767, lon: -96.797, city: "Dallas", region: "Texas", country: "US", postalCode: "75201", timezone: "America/Chicago" },
  "Phoenix": { lat: 33.4484, lon: -112.074, city: "Phoenix", region: "Arizona", country: "US", postalCode: "85001", timezone: "America/Phoenix" },
  "Las Vegas": { lat: 36.1699, lon: -115.1398, city: "Las Vegas", region: "Nevada", country: "US", postalCode: "89101", timezone: "America/Los_Angeles" },
  "Washington DC": { lat: 38.9072, lon: -77.0369, city: "Washington DC", region: "District of Columbia", country: "US", postalCode: "20001", timezone: "America/New_York" },
  "Honolulu": { lat: 21.3069, lon: -157.8583, city: "Honolulu", region: "Hawaii", country: "US", postalCode: "96801", timezone: "Pacific/Honolulu" },
  "Anchorage": { lat: 61.2181, lon: -149.9003, city: "Anchorage", region: "Alaska", country: "US", postalCode: "99501", timezone: "America/Anchorage" },
  "Vancouver": { lat: 49.2827, lon: -123.1207, city: "Vancouver", region: "British Columbia", country: "CA", postalCode: "V5K", timezone: "America/Vancouver" },
  "Montreal": { lat: 45.5019, lon: -73.5674, city: "Montreal", region: "Quebec", country: "CA", postalCode: "H1A", timezone: "America/Toronto" },
  "Ottawa": { lat: 45.4215, lon: -75.6972, city: "Ottawa", region: "Ontario", country: "CA", postalCode: "K1A", timezone: "America/Toronto" },
  "Calgary": { lat: 51.0447, lon: -114.0719, city: "Calgary", region: "Alberta", country: "CA", postalCode: "T2P", timezone: "America/Edmonton" },
  "Havana": { lat: 23.1136, lon: -82.3666, city: "Havana", region: "La Habana", country: "CU", postalCode: "10100", timezone: "America/Havana" },
  "Kingston": { lat: 17.9714, lon: -76.7931, city: "Kingston", region: "Kingston", country: "JM", postalCode: "JMAAW01", timezone: "America/Jamaica" },
  "San Jose CR": { lat: 9.9281, lon: -84.0907, city: "San Jose CR", region: "San Jose", country: "CR", postalCode: "10101", timezone: "America/Costa_Rica" },
  "Panama City": { lat: 8.9824, lon: -79.5199, city: "Panama City", region: "Panama", country: "PA", postalCode: "0801", timezone: "America/Panama" },
  "Guatemala City": { lat: 14.6349, lon: -90.5069, city: "Guatemala City", region: "Guatemala", country: "GT", postalCode: "01001", timezone: "America/Guatemala" },
  "Buenos Aires": { lat: -34.6037, lon: -58.3816, city: "Buenos Aires", region: "Buenos Aires", country: "AR", postalCode: "C1001", timezone: "America/Argentina/Buenos_Aires" },
  "Rio de Janeiro": { lat: -22.9068, lon: -43.1729, city: "Rio de Janeiro", region: "Rio de Janeiro", country: "BR", postalCode: "20000-000", timezone: "America/Sao_Paulo" },
  "Lima": { lat: -12.0464, lon: -77.0428, city: "Lima", region: "Lima", country: "PE", postalCode: "15001", timezone: "America/Lima" },
  "Bogota": { lat: 4.711, lon: -74.0721, city: "Bogota", region: "Bogota", country: "CO", postalCode: "110111", timezone: "America/Bogota" },
  "Santiago": { lat: -33.4489, lon: -70.6693, city: "Santiago", region: "Santiago Metropolitan", country: "CL", postalCode: "8320000", timezone: "America/Santiago" },
  "Caracas": { lat: 10.4806, lon: -66.9036, city: "Caracas", region: "Distrito Capital", country: "VE", postalCode: "1010", timezone: "America/Caracas" },
  "Quito": { lat: -0.1807, lon: -78.4678, city: "Quito", region: "Pichincha", country: "EC", postalCode: "170150", timezone: "America/Guayaquil" },
  "Montevideo": { lat: -34.9011, lon: -56.1645, city: "Montevideo", region: "Montevideo", country: "UY", postalCode: "11000", timezone: "America/Montevideo" },
  "La Paz": { lat: -16.4897, lon: -68.1193, city: "La Paz", region: "La Paz", country: "BO", postalCode: "0201", timezone: "America/La_Paz" },
  "Asuncion": { lat: -25.2637, lon: -57.5759, city: "Asuncion", region: "Asuncion", country: "PY", postalCode: "1209", timezone: "America/Asuncion" },
  "Rome": { lat: 41.9028, lon: 12.4964, city: "Rome", region: "Lazio", country: "IT", postalCode: "00100", timezone: "Europe/Rome" },
  "Madrid": { lat: 40.4168, lon: -3.7038, city: "Madrid", region: "Madrid", country: "ES", postalCode: "28001", timezone: "Europe/Madrid" },
  "Barcelona": { lat: 41.3874, lon: 2.1686, city: "Barcelona", region: "Catalonia", country: "ES", postalCode: "08001", timezone: "Europe/Madrid" },
  "Amsterdam": { lat: 52.3676, lon: 4.9041, city: "Amsterdam", region: "North Holland", country: "NL", postalCode: "1011", timezone: "Europe/Amsterdam" },
  "Brussels": { lat: 50.8503, lon: 4.3517, city: "Brussels", region: "Brussels", country: "BE", postalCode: "1000", timezone: "Europe/Brussels" },
  "Vienna": { lat: 48.2082, lon: 16.3738, city: "Vienna", region: "Vienna", country: "AT", postalCode: "1010", timezone: "Europe/Vienna" },
  "Zurich": { lat: 47.3769, lon: 8.5417, city: "Zurich", region: "Zurich", country: "CH", postalCode: "8001", timezone: "Europe/Zurich" },
  "Geneva": { lat: 46.2044, lon: 6.1432, city: "Geneva", region: "Geneva", country: "CH", postalCode: "1201", timezone: "Europe/Zurich" },
  "Stockholm": { lat: 59.3293, lon: 18.0686, city: "Stockholm", region: "Stockholm", country: "SE", postalCode: "11121", timezone: "Europe/Stockholm" },
  "Oslo": { lat: 59.9139, lon: 10.7522, city: "Oslo", region: "Oslo", country: "NO", postalCode: "0150", timezone: "Europe/Oslo" },
  "Copenhagen": { lat: 55.6761, lon: 12.5683, city: "Copenhagen", region: "Capital Region", country: "DK", postalCode: "1050", timezone: "Europe/Copenhagen" },
  "Helsinki": { lat: 60.1699, lon: 24.9384, city: "Helsinki", region: "Uusimaa", country: "FI", postalCode: "00100", timezone: "Europe/Helsinki" },
  "Dublin": { lat: 53.3498, lon: -6.2603, city: "Dublin", region: "Leinster", country: "IE", postalCode: "D01", timezone: "Europe/Dublin" },
  "Lisbon": { lat: 38.7223, lon: -9.1393, city: "Lisbon", region: "Lisbon", country: "PT", postalCode: "1000-001", timezone: "Europe/Lisbon" },
  "Warsaw": { lat: 52.2297, lon: 21.0122, city: "Warsaw", region: "Masovian", country: "PL", postalCode: "00-001", timezone: "Europe/Warsaw" },
  "Prague": { lat: 50.0755, lon: 14.4378, city: "Prague", region: "Prague", country: "CZ", postalCode: "11000", timezone: "Europe/Prague" },
  "Budapest": { lat: 47.4979, lon: 19.0402, city: "Budapest", region: "Budapest", country: "HU", postalCode: "1011", timezone: "Europe/Budapest" },
  "Athens": { lat: 37.9838, lon: 23.7275, city: "Athens", region: "Attica", country: "GR", postalCode: "10551", timezone: "Europe/Athens" },
  "Istanbul": { lat: 41.0082, lon: 28.9784, city: "Istanbul", region: "Istanbul", country: "TR", postalCode: "34000", timezone: "Europe/Istanbul" },
  "Moscow": { lat: 55.7558, lon: 37.6173, city: "Moscow", region: "Moscow", country: "RU", postalCode: "101000", timezone: "Europe/Moscow" },
  "Kyiv": { lat: 50.4501, lon: 30.5234, city: "Kyiv", region: "Kyiv", country: "UA", postalCode: "01001", timezone: "Europe/Kyiv" },
  "Bucharest": { lat: 44.4268, lon: 26.1025, city: "Bucharest", region: "Bucharest", country: "RO", postalCode: "010011", timezone: "Europe/Bucharest" },
  "Sofia": { lat: 42.6977, lon: 23.3219, city: "Sofia", region: "Sofia", country: "BG", postalCode: "1000", timezone: "Europe/Sofia" },
  "Zagreb": { lat: 45.815, lon: 15.9819, city: "Zagreb", region: "Zagreb", country: "HR", postalCode: "10000", timezone: "Europe/Zagreb" },
  "Belgrade": { lat: 44.7866, lon: 20.4489, city: "Belgrade", region: "Belgrade", country: "RS", postalCode: "11000", timezone: "Europe/Belgrade" },
  "Reykjavik": { lat: 64.1466, lon: -21.9426, city: "Reykjavik", region: "Capital Region", country: "IS", postalCode: "101", timezone: "Atlantic/Reykjavik" },
  "Edinburgh": { lat: 55.9533, lon: -3.1883, city: "Edinburgh", region: "Scotland", country: "GB", postalCode: "EH1", timezone: "Europe/London" },
  "Manchester": { lat: 53.4808, lon: -2.2426, city: "Manchester", region: "England", country: "GB", postalCode: "M1", timezone: "Europe/London" },
  "Munich": { lat: 48.1351, lon: 11.582, city: "Munich", region: "Bavaria", country: "DE", postalCode: "80331", timezone: "Europe/Berlin" },
  "Frankfurt": { lat: 50.1109, lon: 8.6821, city: "Frankfurt", region: "Hesse", country: "DE", postalCode: "60306", timezone: "Europe/Berlin" },
  "Hamburg": { lat: 53.5511, lon: 9.9937, city: "Hamburg", region: "Hamburg", country: "DE", postalCode: "20095", timezone: "Europe/Berlin" },
  "Milan": { lat: 45.4642, lon: 9.19, city: "Milan", region: "Lombardy", country: "IT", postalCode: "20121", timezone: "Europe/Rome" },
  "Dubai": { lat: 25.2048, lon: 55.2708, city: "Dubai", region: "Dubai", country: "AE", postalCode: "00000", timezone: "Asia/Dubai" },
  "Abu Dhabi": { lat: 24.4539, lon: 54.3773, city: "Abu Dhabi", region: "Abu Dhabi", country: "AE", postalCode: "00000", timezone: "Asia/Dubai" },
  "Tel Aviv": { lat: 32.0853, lon: 34.7818, city: "Tel Aviv", region: "Tel Aviv", country: "IL", postalCode: "6100000", timezone: "Asia/Jerusalem" },
  "Jerusalem": { lat: 31.7683, lon: 35.2137, city: "Jerusalem", region: "Jerusalem", country: "IL", postalCode: "9100000", timezone: "Asia/Jerusalem" },
  "Riyadh": { lat: 24.7136, lon: 46.6753, city: "Riyadh", region: "Riyadh", country: "SA", postalCode: "11564", timezone: "Asia/Riyadh" },
  "Doha": { lat: 25.2854, lon: 51.531, city: "Doha", region: "Doha", country: "QA", postalCode: "00000", timezone: "Asia/Qatar" },
  "Amman": { lat: 31.9454, lon: 35.9284, city: "Amman", region: "Amman", country: "JO", postalCode: "11118", timezone: "Asia/Amman" },
  "Beirut": { lat: 33.8938, lon: 35.5018, city: "Beirut", region: "Beirut", country: "LB", postalCode: "1100", timezone: "Asia/Beirut" },
  "Baghdad": { lat: 33.3152, lon: 44.3661, city: "Baghdad", region: "Baghdad", country: "IQ", postalCode: "10001", timezone: "Asia/Baghdad" },
  "Tehran": { lat: 35.6892, lon: 51.389, city: "Tehran", region: "Tehran", country: "IR", postalCode: "1111", timezone: "Asia/Tehran" },
  "Lagos": { lat: 6.5244, lon: 3.3792, city: "Lagos", region: "Lagos", country: "NG", postalCode: "100001", timezone: "Africa/Lagos" },
  "Nairobi": { lat: -1.2921, lon: 36.8219, city: "Nairobi", region: "Nairobi", country: "KE", postalCode: "00100", timezone: "Africa/Nairobi" },
  "Johannesburg": { lat: -26.2041, lon: 28.0473, city: "Johannesburg", region: "Gauteng", country: "ZA", postalCode: "2000", timezone: "Africa/Johannesburg" },
  "Casablanca": { lat: 33.5731, lon: -7.5898, city: "Casablanca", region: "Casablanca-Settat", country: "MA", postalCode: "20000", timezone: "Africa/Casablanca" },
  "Addis Ababa": { lat: 9.03, lon: 38.74, city: "Addis Ababa", region: "Addis Ababa", country: "ET", postalCode: "1000", timezone: "Africa/Addis_Ababa" },
  "Accra": { lat: 5.6037, lon: -0.187, city: "Accra", region: "Greater Accra", country: "GH", postalCode: "00233", timezone: "Africa/Accra" },
  "Dakar": { lat: 14.7167, lon: -17.4677, city: "Dakar", region: "Dakar", country: "SN", postalCode: "10000", timezone: "Africa/Dakar" },
  "Tunis": { lat: 36.8065, lon: 10.1815, city: "Tunis", region: "Tunis", country: "TN", postalCode: "1000", timezone: "Africa/Tunis" },
  "Algiers": { lat: 36.7538, lon: 3.0588, city: "Algiers", region: "Algiers", country: "DZ", postalCode: "16000", timezone: "Africa/Algiers" },
  "Kinshasa": { lat: -4.4419, lon: 15.2663, city: "Kinshasa", region: "Kinshasa", country: "CD", postalCode: "00000", timezone: "Africa/Kinshasa" },
  "Beijing": { lat: 39.9042, lon: 116.4074, city: "Beijing", region: "Beijing", country: "CN", postalCode: "100000", timezone: "Asia/Shanghai" },
  "Shanghai": { lat: 31.2304, lon: 121.4737, city: "Shanghai", region: "Shanghai", country: "CN", postalCode: "200000", timezone: "Asia/Shanghai" },
  "Hong Kong": { lat: 22.3193, lon: 114.1694, city: "Hong Kong", region: "Hong Kong", country: "HK", postalCode: "999077", timezone: "Asia/Hong_Kong" },
  "Taipei": { lat: 25.033, lon: 121.5654, city: "Taipei", region: "Taipei", country: "TW", postalCode: "100", timezone: "Asia/Taipei" },
  "Bangkok": { lat: 13.7563, lon: 100.5018, city: "Bangkok", region: "Bangkok", country: "TH", postalCode: "10200", timezone: "Asia/Bangkok" },
  "Jakarta": { lat: -6.2088, lon: 106.8456, city: "Jakarta", region: "Jakarta", country: "ID", postalCode: "10110", timezone: "Asia/Jakarta" },
  "Manila": { lat: 14.5995, lon: 120.9842, city: "Manila", region: "Metro Manila", country: "PH", postalCode: "1000", timezone: "Asia/Manila" },
  "Kuala Lumpur": { lat: 3.139, lon: 101.6869, city: "Kuala Lumpur", region: "Kuala Lumpur", country: "MY", postalCode: "50000", timezone: "Asia/Kuala_Lumpur" },
  "Hanoi": { lat: 21.0278, lon: 105.8342, city: "Hanoi", region: "Hanoi", country: "VN", postalCode: "100000", timezone: "Asia/Ho_Chi_Minh" },
  "Ho Chi Minh City": { lat: 10.8231, lon: 106.6297, city: "Ho Chi Minh City", region: "Ho Chi Minh", country: "VN", postalCode: "700000", timezone: "Asia/Ho_Chi_Minh" },
  "New Delhi": { lat: 28.6139, lon: 77.209, city: "New Delhi", region: "Delhi", country: "IN", postalCode: "110001", timezone: "Asia/Kolkata" },
  "Bangalore": { lat: 12.9716, lon: 77.5946, city: "Bangalore", region: "Karnataka", country: "IN", postalCode: "560001", timezone: "Asia/Kolkata" },
  "Kolkata": { lat: 22.5726, lon: 88.3639, city: "Kolkata", region: "West Bengal", country: "IN", postalCode: "700001", timezone: "Asia/Kolkata" },
  "Chennai": { lat: 13.0827, lon: 80.2707, city: "Chennai", region: "Tamil Nadu", country: "IN", postalCode: "600001", timezone: "Asia/Kolkata" },
  "Karachi": { lat: 24.8607, lon: 67.0011, city: "Karachi", region: "Sindh", country: "PK", postalCode: "74200", timezone: "Asia/Karachi" },
  "Lahore": { lat: 31.5497, lon: 74.3436, city: "Lahore", region: "Punjab", country: "PK", postalCode: "54000", timezone: "Asia/Karachi" },
  "Dhaka": { lat: 23.8103, lon: 90.4125, city: "Dhaka", region: "Dhaka", country: "BD", postalCode: "1000", timezone: "Asia/Dhaka" },
  "Colombo": { lat: 6.9271, lon: 79.8612, city: "Colombo", region: "Western", country: "LK", postalCode: "00100", timezone: "Asia/Colombo" },
  "Kathmandu": { lat: 27.7172, lon: 85.324, city: "Kathmandu", region: "Bagmati", country: "NP", postalCode: "44600", timezone: "Asia/Kathmandu" },
  "Melbourne": { lat: -37.8136, lon: 144.9631, city: "Melbourne", region: "Victoria", country: "AU", postalCode: "3000", timezone: "Australia/Melbourne" },
  "Brisbane": { lat: -27.4698, lon: 153.0251, city: "Brisbane", region: "Queensland", country: "AU", postalCode: "4000", timezone: "Australia/Brisbane" },
  "Perth": { lat: -31.9505, lon: 115.8605, city: "Perth", region: "Western Australia", country: "AU", postalCode: "6000", timezone: "Australia/Perth" },
  "Auckland": { lat: -36.8485, lon: 174.7633, city: "Auckland", region: "Auckland", country: "NZ", postalCode: "1010", timezone: "Pacific/Auckland" },
  "Wellington": { lat: -41.2865, lon: 174.7762, city: "Wellington", region: "Wellington", country: "NZ", postalCode: "6011", timezone: "Pacific/Auckland" },
};

function resolveGeo(cityName, override) {
  const base = cityName && CITY_PRESETS[cityName]
    ? CITY_PRESETS[cityName]
    : { lat: null, lon: null, city: null, region: null, postalCode: null, country: null, timezone: null };
  return { ...base, ...(override || {}) };
}

export default {
  async fetch(request, env) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: CORS_HEADERS });
    }

    const url = new URL(request.url);
    const parts = url.pathname.split('/').filter(Boolean);

    // ---- gallery/events: recent delivery events across ALL lamps, most
    // recent first — the data feed the future Gallery/map page will read.
    // Shape of this may evolve once that page actually gets designed. ----
    if (parts[0] === 'gallery' && parts[1] === 'events' && request.method === 'GET') {
      const limit = Math.min(200, parseInt(url.searchParams.get('limit') || '50', 10) || 50);
      const listing = await env.LAMP_KV.list({ prefix: 'event:', limit });
      const events = await Promise.all(listing.keys.map(async k => {
        const raw = await env.LAMP_KV.get(k.name);
        return raw ? JSON.parse(raw) : null;
      }));
      return json({ events: events.filter(Boolean) });
    }

    // ---- gallery/cities: lists the preset city names /gallery/seed accepts ----
    if (parts[0] === 'gallery' && parts[1] === 'cities' && request.method === 'GET') {
      return json({ cities: Object.keys(CITY_PRESETS) });
    }

    // ---- gallery/seed: writes a fake event record for testing the Gallery
    // without real globally-distributed devices. Body: { lampId?,
    // sentCity?, downloadCity?, sentGeo?, downloadGeo?, deliverySeconds? }.
    // City names come from /gallery/cities; sentGeo/downloadGeo can override
    // or fully replace a preset with custom coordinates. ----
    if (parts[0] === 'gallery' && parts[1] === 'seed' && request.method === 'POST') {
      const body = await request.json().catch(() => ({}));
      const lampId = body.lampId || 'test-lamp-1';
      // any of these can combine: a city name fills in city/region/country/
      // timezone as a base, sentGeo/downloadGeo can override any field
      // wholesale, and sentLat/sentLon/downloadLat/downloadLon are a
      // shorthand for just the coordinates — no city needed at all if you
      // just want to drop a pin at an exact lat/lon
      const sentGeo = resolveGeo(body.sentCity, {
        ...(body.sentGeo || {}),
        ...(body.sentLat != null ? { lat: body.sentLat } : {}),
        ...(body.sentLon != null ? { lon: body.sentLon } : {}),
      });
      const downloadGeo = resolveGeo(body.downloadCity, {
        ...(body.downloadGeo || {}),
        ...(body.downloadLat != null ? { lat: body.downloadLat } : {}),
        ...(body.downloadLon != null ? { lon: body.downloadLon } : {}),
      });
      const deliveryMs = body.deliverySeconds != null
        ? Math.round(body.deliverySeconds * 1000)
        : Math.round(30000 + Math.random() * 600000); // random 30s-10min if not specified
      const downloadedAt = Date.now();
      const sentAt = downloadedAt - deliveryMs;
      const itemId = `${sentAt}-${crypto.randomUUID().slice(0, 8)}.png`;
      const reverseTs = String(MAX_TS - sentAt).padStart(14, '0');
      const eventKey = `event:${reverseTs}:${lampId}:${itemId}`;
      const record = { lampId, itemId, sentAt, sentGeo, downloadedAt, downloadGeo, deliveryMs };
      await env.LAMP_KV.put(eventKey, JSON.stringify(record));
      return json({ ok: true, event: record });
    }

    // ============================================================
    // PUBLIC IMAGE GALLERY: a rolling set of images tagged by an arbitrary
    // galleryId (e.g. 'test-gallery') — separate from the per-lamp display
    // queue below. These PERSIST: fetching one does not delete it, unlike
    // the lamp queue which is consumed on ack. Meant for something that
    // pulls the same images repeatedly (e.g. a scrolling display), not a
    // one-shot delivery pipeline. Same R2 bucket as the lamp queue
    // (env.LAMP_IMAGES), just a different key prefix
    // (gallery-images/{galleryId}/{itemId}), so no new binding is needed.
    // No auth on any of this yet — same caveat as everything else here.
    // ============================================================
    if (parts[0] === 'gallery' && parts.length >= 2 && !['events', 'cities', 'seed'].includes(parts[1])) {
      const galleryId = parts[1];
      const action = parts[2];

      if (action === 'upload' && request.method === 'POST') {
        const contentType = request.headers.get('Content-Type') || 'image/png';
        const ext = extFromContentType(contentType);
        const bytes = await request.arrayBuffer();
        const itemId = `${Date.now()}-${crypto.randomUUID().slice(0, 8)}.${ext}`;
        await env.LAMP_IMAGES.put(`gallery-images/${galleryId}/${itemId}`, bytes, {
          httpMetadata: { contentType: contentTypeFromExt(ext) },
        });
        return json({ ok: true, galleryId, itemId });
      }

      // ---- images: GET lists everything currently in this gallery;
      // DELETE clears the whole thing in one shot (dev/testing
      // convenience, same as the lamp queue's bulk DELETE below) ----
      if (action === 'images' && parts.length === 3 && request.method === 'GET') {
        const items = await getGalleryImageItems(env, galleryId);
        return json({
          galleryId,
          count: items.length,
          items: items.map(obj => ({
            itemId: obj.key.slice(`gallery-images/${galleryId}/`.length),
            size: obj.size,
            uploadedAt: obj.uploaded ? obj.uploaded.getTime() : null,
          })),
        });
      }

      if (action === 'images' && parts.length === 3 && request.method === 'DELETE') {
        const items = await getGalleryImageItems(env, galleryId);
        await Promise.all(items.map(obj => env.LAMP_IMAGES.delete(obj.key)));
        return json({ ok: true, deleted: items.length });
      }

      // ---- images/{itemId}: GET fetches one image's raw bytes (does NOT
      // remove it — this gallery is read repeatedly, not consumed); DELETE
      // removes just that one item ----
      if (action === 'images' && parts.length === 4) {
        const itemId = decodeURIComponent(parts[3]);
        const key = `gallery-images/${galleryId}/${itemId}`;
        const ext = (itemId.split('.').pop() || 'png').toLowerCase();

        if (request.method === 'GET') {
          const obj = await env.LAMP_IMAGES.get(key);
          if (!obj) return new Response(null, { status: 404, headers: CORS_HEADERS });
          return new Response(obj.body, {
            headers: {
              ...CORS_HEADERS,
              'Content-Type': contentTypeFromExt(ext),
              'Cache-Control': 'public, max-age=3600',
            },
          });
        }

        if (request.method === 'DELETE') {
          await env.LAMP_IMAGES.delete(key);
          return json({ ok: true });
        }
      }
    }

    if (parts[0] !== 'lamp' || parts.length < 3) {
      return new Response('Not found', { status: 404, headers: CORS_HEADERS });
    }

    const lampId = parts[1];
    const action = parts[2];

    // ---- upload: enqueue a new item (still image or animation bundle — both
    // are just one PNG blob at this layer) ----
    if (action === 'upload' && request.method === 'POST') {
      const items = await getQueueItems(env, lampId);
      if (items.length >= MAX_QUEUE_DEPTH) {
        return json({ error: 'queue_full', queueDepth: items.length }, 409);
      }
      const bytes = await request.arrayBuffer();
      const sentAt = Date.now();
      const itemId = `${sentAt}-${crypto.randomUUID().slice(0, 8)}.png`;
      await env.LAMP_IMAGES.put(`queue/${lampId}/${itemId}`, bytes, {
        httpMetadata: { contentType: 'image/png' },
      });
      await env.LAMP_KV.put(`meta:${lampId}`, JSON.stringify({ lastUpload: sentAt }));
      await recordSentEvent(env, lampId, itemId, request, sentAt);
      return json({ ok: true, itemId, queueDepth: items.length + 1 });
    }

    // ---- status: last-sent timestamp + live queue depth, so the frontend
    // can show "queue full" before someone even tries to send ----
    if (action === 'status' && request.method === 'GET') {
      const metaRaw = await env.LAMP_KV.get(`meta:${lampId}`);
      const meta = metaRaw ? JSON.parse(metaRaw) : {};
      const items = await getQueueItems(env, lampId);
      return json({
        lastUpload: meta.lastUpload || null,
        queueDepth: items.length,
        queueFull: items.length >= MAX_QUEUE_DEPTH,
      });
    }

    // ---- queue: GET lists pending items; DELETE clears the whole queue in
    // one shot. The bulk DELETE is a dev/testing convenience — real firmware
    // usage should still ack one real download at a time via DELETE
    // /items/{itemId}, not wipe everything blind. ----
    if (action === 'queue' && request.method === 'GET') {
      const items = await getQueueItems(env, lampId);
      return json({
        queueDepth: items.length,
        items: items.map(obj => ({
          itemId: obj.key.slice(`queue/${lampId}/`.length),
          size: obj.size,
        })),
      });
    }

    if (action === 'queue' && request.method === 'DELETE') {
      const items = await getQueueItems(env, lampId);
      await Promise.all(items.map(obj => env.LAMP_IMAGES.delete(obj.key)));
      return json({ ok: true, deleted: items.length, queueDepth: 0 });
    }

    // ---- items/{itemId}: GET fetches one item's raw bytes (and records the
    // download-side geo/timing event); DELETE acks it, freeing the slot.
    // Firmware calls GET then DELETE right after a successful download —
    // "downloaded" and "displayed" are two different states it tracks
    // itself, the Worker only needs to know "downloaded" to free the slot
    // and to log the delivery event. ----
    if (action === 'items' && parts.length === 4) {
      const itemId = decodeURIComponent(parts[3]);
      const key = `queue/${lampId}/${itemId}`;

      if (request.method === 'GET') {
        const obj = await env.LAMP_IMAGES.get(key);
        if (!obj) return new Response(null, { status: 404, headers: CORS_HEADERS });
        await recordDownloadEvent(env, lampId, itemId, request);
        return new Response(obj.body, {
          headers: { ...CORS_HEADERS, 'Content-Type': 'image/png', 'X-Item-Id': itemId },
        });
      }

      if (request.method === 'DELETE') {
        await env.LAMP_IMAGES.delete(key);
        const items = await getQueueItems(env, lampId);
        return json({ ok: true, queueDepth: items.length });
      }
    }

    // ---- next: pulls just the single oldest item — kept around as a
    // simpler option for quick tests / debugging, not the main firmware flow
    // now that /queue + per-item GET exist ----
    if (action === 'next' && request.method === 'GET') {
      const items = await getQueueItems(env, lampId);
      if (items.length === 0) {
        return new Response(null, { status: 204, headers: CORS_HEADERS });
      }
      const oldest = items[0];
      const obj = await env.LAMP_IMAGES.get(oldest.key);
      if (!obj) return new Response(null, { status: 204, headers: CORS_HEADERS });
      const itemId = oldest.key.slice(`queue/${lampId}/`.length);
      await recordDownloadEvent(env, lampId, itemId, request);
      return new Response(obj.body, {
        headers: {
          ...CORS_HEADERS,
          'Content-Type': 'image/png',
          'X-Item-Id': itemId,
          'X-Queue-Depth': String(items.length),
        },
      });
    }

    return new Response('Not found', { status: 404, headers: CORS_HEADERS });
  },
};