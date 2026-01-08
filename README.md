# Build Work Space Under PaCC/
```
colcon build --symlink-install
source install/setup.bash
```

# Run Ingestion
```
ros2 run bring_up ingest
```

# Query and Send UDP
```
python3 retrive_main.py --topic /camera/image_raw --start-ns 1766097953826221833 --end-ns 1766097978540381848 --port 9000
```