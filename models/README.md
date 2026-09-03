# models/

## yolov8n.onnx

Person/object detector used by the camera privacy stage
(`src/privacy/human_blur.cpp`). Only the `person` class (COCO id 0) is acted on;
every detected person is Gaussian-blurred before the frame leaves pDAL.

- **Source:** exported from Ultralytics `yolov8n.pt` (COCO, `imgsz=640`,
  `opset=12`, simplified). Output tensor `[1, 84, 8400]` (4 box + 80 classes,
  no NMS).
- **License:** the weights are **AGPL-3.0** (Ultralytics). See
  <https://github.com/ultralytics/ultralytics/blob/main/LICENSE>. Using them in
  a distributed service has AGPL implications; swap in a permissively licensed
  detector for anything shipped.
- **Re-export:**
  ```bash
  pip install ultralytics
  yolo export model=yolov8n.pt format=onnx opset=12 imgsz=640 simplify=True nms=False
  mv yolov8n.onnx models/yolov8n.onnx
  ```

The ONNX Runtime that executes this model is **not** in the repo; run
`scripts/fetch_privacy_deps.sh` to place it under `third_party/onnxruntime/`.
