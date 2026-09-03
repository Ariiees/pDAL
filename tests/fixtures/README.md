# Test fixtures

- `street_two_people.jpg` — one frame from the KITTI raw dataset (road scene
  with two pedestrians and a cyclist). Used by `pdal_privacy_tests` to check
  that people are detected and blurred. KITTI is CC BY-NC-SA 3.0; included here
  only as a test input.
- `blank_scene.jpg` — synthetic solid-grey frame with no people; checks that a
  people-free frame yields `regions_blurred == 0` and unchanged dimensions.
