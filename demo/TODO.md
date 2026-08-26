# Future real hard-brake event integration

The OEM demo intentionally returns `events: []`. Current AVS recordings do not
contain Lincoln MKZ / Dataspeed brake records, so existing GPS-derived event
summaries are not used and no incidents are fabricated.

The future implementation must follow this sequence:

1. Add an AVS recorder that subscribes to the real Lincoln MKZ / Dataspeed
   vehicle-interface brake signal and stores each value with its source
   timestamp.
2. After AVS closes a recording, invoke a pDAL event scanner over those stored
   brake records.
3. Document and version a hard-brake algorithm and threshold based on the real
   brake signal. The scanner must reject missing, stale, and invalid source
   values rather than substituting simulated data.
4. Group qualifying samples into hard-brake intervals and choose the peak
   braking sample timestamp as the incident time.
5. Add only minimal incident metadata (event ID/type, interval, peak timestamp,
   threshold/algorithm version, and non-sensitive metrics) to the trip summary.
6. Show incident markers on the OEM map and timeline. Let the Incident
   Investigator jump to a pDAL-authorized time window around an event; other
   roles must still be subject to their normal resource policy.

The event scanner and viewer integration remain blocked until the real brake
recorder and a documented signal-specific threshold are available.
