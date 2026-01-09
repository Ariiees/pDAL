# Build Work Space Under PaCC/
```
colcon build --symlink-install
source install/setup.bash
```

# Run Ingestion
```
ros2 run bring_up ingest
```
It lisen to a ros2 topics shown in avs/config/topics.yaml file

# Query and Send UDP
```
python3 retrive_main.py --topic /camera/image_raw --start-ns 1766097953826221833 --end-ns 1766097978540381848 --port 9000
```

On host side, run the following to parse UDP pack into each encrpted records to consumer
```
python3 host_udp_parser.py --bind-ip 0.0.0.0 --port 9000 --verbose
```