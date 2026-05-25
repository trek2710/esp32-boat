# One ESP runs softAP; other ESPs and the iPhone join it

For the boat deployment, the Bridge ESP (or whichever ESP is most reliably
powered) brings up a softAP with a known SSID; other ESPs and the iPhone
connect to it as stations. We considered relying on an existing boat WiFi
router (cleaner, but assumes hardware that's not always present at
anchor) and a try-station-then-softAP hybrid (more robust but adds
firmware mode-switching logic). softAP-only keeps the deployment
self-contained at the cost of bench-test inconvenience — the laptop loses
internet while joined to the boat AP — which the user accepted because iOS
testing is the bench-testing pattern anyway.
