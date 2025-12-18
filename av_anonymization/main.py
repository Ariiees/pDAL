"""Main entry point for the detection-reduction pipeline."""

import argparse
import cv2
from pathlib import Path

from loader import ImageSequenceLoader
from pipeline import DetectionReductionPipeline
from visualizer import Visualizer
from policies import get_default_policies, get_policy_names, get_policy_description
from utils import ensure_dir, save_json, format_statistics, print_progress


def process_dataset(data_dir: str, output_dir: str, policy_id: str,
                   num_frames: int = None, save_comparison: bool = True,
                   use_openvino: bool = False):
    """
    Process dataset with specified policy.
    
    Args:
        data_dir: Path to image directory
        output_dir: Path to save results
        policy_id: ID of policy to apply
        num_frames: Number of frames to process (None = all)
        save_comparison: Whether to save comparison images
        use_openvino: Whether to use OpenVINO optimization
    """
    # Create output directories
    output_path = ensure_dir(output_dir)
    ensure_dir(output_path / "frames")
    ensure_dir(output_path / "metadata")
    if save_comparison:
        ensure_dir(output_path / "comparisons")
    
    # Initialize components
    print("Initializing pipeline...")
    loader = ImageSequenceLoader(data_dir)
    pipeline = DetectionReductionPipeline("yolov8n.pt", use_openvino=use_openvino)
    
    # Register policies
    policies = get_default_policies()
    for pid, policy_data in policies.items():
        pipeline.register_policy(pid, policy_data["reduction_rules"])
    
    if policy_id not in pipeline.policies:
        print(f"Error: Policy '{policy_id}' not found")
        print(f"Available: {get_policy_names()}")
        return
    
    frames_to_process = num_frames if num_frames else len(loader)
    print(f"\nProcessing {frames_to_process} frames with policy: {policy_id}")
    print(f"Description: {get_policy_description(policy_id)}")
    print("-" * 60)
    
    for i, frame_data in enumerate(loader):
        if num_frames and i >= num_frames:
            break
        
        frame_id = frame_data["frame_id"]
        original_frame = frame_data["frame"]
        
        # Process frame
        result = pipeline.process(original_frame, policy_id)
        
        # Print progress
        print_progress(i + 1, frames_to_process, frame_id, result)
        
        # Save processed frame
        cv2.imwrite(
            str(output_path / "frames" / f"{frame_id}.jpg"), 
            result["frame"]
        )
        
        # Save metadata (including timing)
        save_json({
            "frame_id": frame_id,
            "policy_id": policy_id,
            "objects": result["metadata"],
            "summary": {
                "detections": result["detections"],
                "kept": result["kept"],
                "anonymized": result["anonymized"],
                "removed": result["removed"]
            },
            "timing": result["timing"]
        }, str(output_path / "metadata" / f"{frame_id}.json"))
        
        # Save comparison
        if save_comparison:
            comparison = Visualizer.create_comparison(
                original_frame, result["frame"], result["metadata"]
            )
            cv2.imwrite(
                str(output_path / "comparisons" / f"{frame_id}.jpg"), 
                comparison
            )
    
    # Print and save statistics
    stats = pipeline.get_statistics()
    print("\n" + format_statistics(stats))
    print(f"Results saved to: {output_dir}")
    
    save_json({
        "policy_id": policy_id,
        "description": get_policy_description(policy_id),
        "statistics": stats
    }, str(output_path / "summary.json"))


def interactive_demo(data_dir: str, use_openvino: bool = False):
    """
    Interactive demo with keyboard controls.
    
    Args:
        data_dir: Path to image directory
        use_openvino: Whether to use OpenVINO optimization
    """
    loader = ImageSequenceLoader(data_dir)
    pipeline = DetectionReductionPipeline("yolov8n.pt", use_openvino=use_openvino)
    
    # Register policies
    policies = get_default_policies()
    policy_names = list(policies.keys())
    
    for pid, policy_data in policies.items():
        pipeline.register_policy(pid, policy_data["reduction_rules"])
    
    print("\n" + "=" * 60)
    print("Interactive Demo")
    print("=" * 60)
    print("Controls:")
    print("  SPACE  - Next frame")
    print("  P      - Previous frame")
    print("  1-4    - Switch policy")
    for i, name in enumerate(policy_names, 1):
        print(f"          {i}: {name}")
    print("  S      - Save current frame")
    print("  Q/ESC  - Quit")
    print("=" * 60)
    
    frame_idx = 0
    policy_idx = 0
    
    while True:
        # Load and process frame
        frame_data = loader.get_frame_by_index(frame_idx)
        current_policy = policy_names[policy_idx]
        result = pipeline.process(frame_data["frame"], current_policy)
        
        # Create visualization
        comparison = Visualizer.create_comparison(
            frame_data["frame"], result["frame"], result["metadata"]
        )
        
        # Add info bar with detailed timing
        stats = pipeline.get_statistics()
        timing = result.get("timing", {})
        
        info = (f"Frame: {frame_data['frame_id']} ({frame_idx+1}/{len(loader)}) | "
                f"Policy: {current_policy} | "
                f"Det: {result['detections']} | "
                f"Time: {timing.get('total_ms', 0):.1f}ms | "
                f"FPS: {stats['fps']:.1f}")
        comparison = Visualizer.add_info_bar(comparison, info)
        
        # Display
        cv2.imshow("Detection + Reduction Demo", comparison)
        
        # Handle input
        key = cv2.waitKey(0) & 0xFF
        
        if key == ord('q') or key == 27:  # Q or ESC
            break
        elif key == ord(' '):  # Space - next
            frame_idx = min(frame_idx + 1, len(loader) - 1)
        elif key == ord('p'):  # P - previous
            frame_idx = max(frame_idx - 1, 0)
        elif key in [ord('1'), ord('2'), ord('3'), ord('4')]:
            new_idx = int(chr(key)) - 1
            if new_idx < len(policy_names):
                policy_idx = new_idx
                print(f"Switched to policy: {policy_names[policy_idx]}")
        elif key == ord('s'):  # Save
            save_path = f"output_{frame_data['frame_id']}_{current_policy}.jpg"
            cv2.imwrite(save_path, comparison)
            print(f"Saved: {save_path}")
    
    cv2.destroyAllWindows()
    
    # Print final statistics
    print("\n" + format_statistics(pipeline.get_statistics()))
    print("Demo ended.")


def list_policies():
    """Print available policies."""
    policies = get_default_policies()
    print("\nAvailable Policies:")
    print("=" * 60)
    for name, config in policies.items():
        print(f"\n{name}:")
        print(f"  Description: {config['description']}")
        print(f"  Rules:")
        for cls, rule in config['reduction_rules'].items():
            print(f"    - {cls}: {rule['action']}", end="")
            if rule.get('method'):
                print(f" ({rule['method']})", end="")
            print()


def main():
    parser = argparse.ArgumentParser(
        description="AV Data Detection + Reduction Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Process all frames with traffic monitoring policy
  python main.py --data_dir ~/dataset/0000 --policy traffic_monitoring

  # Process first 50 frames with full anonymization
  python main.py --data_dir ~/dataset/0000 --policy full_anonymization --num_frames 50

  # Interactive demo
  python main.py --data_dir ~/dataset/0000 --interactive

  # List available policies
  python main.py --list_policies
        """
    )
    
    parser.add_argument("--data_dir", type=str, default="~/dataset/0000",
                       help="Path to image directory")
    parser.add_argument("--output", type=str, default="./output",
                       help="Output directory")
    parser.add_argument("--policy", type=str, default="traffic_monitoring",
                       choices=get_policy_names(),
                       help="Reduction policy to apply")
    parser.add_argument("--num_frames", type=int, default=None,
                       help="Number of frames to process (default: all)")
    parser.add_argument("--no_comparison", action="store_true",
                       help="Don't save comparison images")
    parser.add_argument("--interactive", action="store_true",
                       help="Run interactive demo")
    parser.add_argument("--list_policies", action="store_true",
                       help="List available policies")
    parser.add_argument("--openvino", action="store_true",
                       help="Use OpenVINO for CPU optimization")
    
    args = parser.parse_args()
    
    if args.list_policies:
        list_policies()
    elif args.interactive:
        interactive_demo(args.data_dir, use_openvino=args.openvino)
    else:
        process_dataset(
            data_dir=args.data_dir,
            output_dir=args.output,
            policy_id=args.policy,
            num_frames=args.num_frames,
            save_comparison=not args.no_comparison,
            use_openvino=args.openvino
        )


if __name__ == "__main__":
    main()
# ```

# ---

# ## Example Output

# Now the statistics will show detailed timing breakdown:
# ```
# ============================================================
# Processing Statistics
# ============================================================
# Frames processed:      154
# Total detections:      892
# Total anonymized:      245
# Total removed:         0

# Timing Breakdown:
#   Avg detection:       28.45 ms
#   Avg anonymization:   3.21 ms
#   Avg total:           31.66 ms

# Average FPS:           31.59
# ============================================================