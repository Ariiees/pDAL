# Brake event calibration

Real Dataspeed pedal recording and protected red timeline markers are now
implemented. The Pi detects sustained `pedal_output` values and the viewer
marks the first qualifying sample. Both SSD and HDD use the existing AVS
historical retrieval path.

Remaining: calibrate threshold/duration against timestamped vehicle driving
tests. Current defaults are raw pedal >= 0.30 for 0.20 seconds; gaps over
0.50 seconds split events. These are candidates, not measured-deceleration
classifications. No vehicle-speed dependency is used.
