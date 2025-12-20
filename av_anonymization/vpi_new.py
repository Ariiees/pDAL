# Updated main() function in your VPI code:
from __future__ import annotations

import base64
import json
import os
import time
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Tuple

import requests

JsonDict = Dict[str, Any]


@dataclass(frozen=True)
class Endpoints:
    access_control_url: str
    avs_query_url: str
    pacc_anonymize_url: str


def now_utc() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def new_request_id() -> str:
    return uuid.uuid4().hex


def post_json(url: str, payload: JsonDict, timeout_seconds: float) -> JsonDict:
    r = requests.post(url, json=payload, timeout=timeout_seconds)
    r.raise_for_status()
    return r.json()


def split_request(vpi_request: JsonDict) -> Tuple[JsonDict, JsonDict]:
    query = vpi_request.get("query") or {}

    avs_request: JsonDict = {
        "request_id": vpi_request["request_id"],
        "app_id": vpi_request["app_id"],
        "timestamp": vpi_request["timestamp"],
        "query": {
            "time_range": query.get("time_range"),
            "regions_of_interest": query.get("regions_of_interest", []),
        },
    }

    pacc_instruction: JsonDict = {
        "request_id": vpi_request["request_id"],
        "app_id": vpi_request["app_id"],
        "timestamp": vpi_request["timestamp"],
        "object_queries": query.get("object_queries", []),
        "output_format": vpi_request.get("output_format", {}),
    }

    return avs_request, pacc_instruction


def handle_vpi_request(vpi_request: JsonDict, endpoints: Endpoints, timeout_seconds: float = 12.0) -> JsonDict:
    vpi_request = dict(vpi_request)
    vpi_request["request_id"] = vpi_request.get("request_id") or new_request_id()
    vpi_request["timestamp"] = vpi_request.get("timestamp") or now_utc()

    if not vpi_request.get("app_id"):
        return {"decision": "deny", "reason": "missing_app_id", "request_id": vpi_request["request_id"]}

    access_payload: JsonDict = {
        "request_id": vpi_request["request_id"],
        "app_id": vpi_request["app_id"],
        "timestamp": vpi_request["timestamp"],
        "signature": vpi_request.get("signature", ""),
        "query": vpi_request.get("query", {}),
        "output_format": vpi_request.get("output_format", {}),
    }
    decision = post_json(endpoints.access_control_url, access_payload, timeout_seconds)

    if decision.get("decision") != "allow":
        return {
            "request_id": vpi_request["request_id"],
            "app_id": vpi_request["app_id"],
            "timestamp": vpi_request["timestamp"],
            "decision": "deny",
            "reason": decision.get("reason", "denied"),
        }

    avs_request, pacc_instruction = split_request(vpi_request)

    avs_result = post_json(endpoints.avs_query_url, avs_request, timeout_seconds)

    pacc_payload: JsonDict = {
        "instruction": pacc_instruction,
        "avs_result": avs_result,
        "access_context": decision.get("access_context", {}),
    }
    pacc_result = post_json(endpoints.pacc_anonymize_url, pacc_payload, timeout_seconds)

    return {
        "request_id": vpi_request["request_id"],
        "app_id": vpi_request["app_id"],
        "timestamp": vpi_request["timestamp"],
        "decision": "allow",
        "result": pacc_result,
    }


def lambda_handler(event: JsonDict, context: Any) -> JsonDict:
    endpoints = Endpoints(
        access_control_url=os.environ["ACCESS_CONTROL_URL"],
        avs_query_url=os.environ["AVS_QUERY_URL"],
        pacc_anonymize_url=os.environ["PACC_ANONYMIZE_URL"],
    )
    timeout_seconds = float(os.getenv("REQUEST_TIMEOUT_SECONDS", "12"))

    body = event.get("body") or "{}"
    if event.get("isBase64Encoded"):
        body = base64.b64decode(body).decode("utf8")

    try:
        vpi_request = json.loads(body)
        result = handle_vpi_request(vpi_request, endpoints, timeout_seconds)
        return {"statusCode": 200, "headers": {"Content-Type": "application/json"}, "body": json.dumps(result)}
    except requests.HTTPError as e:
        return {
            "statusCode": 502,
            "headers": {"Content-Type": "application/json"},
            "body": json.dumps({"error": "upstream_http_error", "detail": str(e)}),
        }
    except Exception as e:
        return {
            "statusCode": 500,
            "headers": {"Content-Type": "application/json"},
            "body": json.dumps({"error": "internal_error", "detail": str(e)}),
        }

def main() -> None:
    """
    Google Colab runnable demo with REAL detection pipeline and image output.
    """
    from pacc_integration import create_pacc_anonymize_handler
    
    # Initialize real PaCC service with detection pipeline
    pacc_anonymize = create_pacc_anonymize_handler(
        model_path="yolov8n.pt",
        use_openvino=False,
        output_dir="./pacc_output"
    )

    # Mock services (same as before)
    def mock_access_control(payload: JsonDict) -> JsonDict:
        return {
            "decision": "allow",
            "access_context": {
                # "policy_id": "demo_policy_v1",
                # "approved_objects": ["license_plate", "pedestrian"],
                "approved_objects": ["license_plate"],
            },
        }

    def mock_avs_query(payload: JsonDict) -> JsonDict:
        return {
            "records": [
                {
                    "record_id": "frame_000001",
                    "timestamp": "20251014T090010Z",
                    "ciphertext_ref": "avs://encrypted/blob/000001",
                },
                {
                    "record_id": "frame_000002",
                    "timestamp": "20251014T090020Z",
                    "ciphertext_ref": "avs://encrypted/blob/000002",
                },
            ]
        }

    # VPI request with output format preferences
    vpi_request: JsonDict = {
        "request_id": new_request_id(),
        "app_id": "third_party_app_identifier",
        "timestamp": now_utc(),
        "signature": "DEMO_SIGNATURE",
        "query": {
            "time_range": {
                "start": "20251014T090000Z",
                "end": "20251014T100000Z",
            },
            "regions_of_interest": ["front_camera", "rear_camera"],
            "object_queries": [
                {
                    "query_id": "q1",
                    "object_type": "license_plate",
                    "attributes": {
                        "state": ["CA", "NY"],
                        "confidence_threshold": 0.85,
                    },
                },
                {
                    "query_id": "q2",
                    "object_type": "pedestrian",
                    "attributes": {
                        "age_range": ["adult"],
                    },
                },
            ],
        },
        "output_format": {
            "type": "structured_data",
            "include_confidence_scores": True,
            "coordinate_system": "normalized",
            # Image output preferences
            "include_images": True,
            "image_format": "jpg",  # or "png"
            "image_encoding": "file",  # or "base64"
        },
    }

    print("=== Step 1: Access Control ===")
    decision = mock_access_control(vpi_request)
    print(json.dumps(decision, indent=2))

    if decision["decision"] != "allow":
        print("Request denied")
        return

    print("\n=== Step 2: Split VPI Request ===")
    avs_request, pacc_instruction = split_request(vpi_request)
    
    print("\n=== Step 3: AVS Query ===")
    avs_result = mock_avs_query(avs_request)
    
    print("\n=== Step 4: REAL PaCC Anonymization with Detection Pipeline ===")
    pacc_payload = {
        "instruction": pacc_instruction,
        "avs_result": avs_result,
        "access_context": decision["access_context"],
    }
    
    # Use REAL detection pipeline with image output!
    pacc_result = pacc_anonymize(pacc_payload)
    
    # Print result with image info
    print("PaCC Result:")
    for frame in pacc_result.get("frames", []):
        print(f"\nFrame: {frame['record_id']}")
        print(f"  Objects detected: {len(frame['objects'])}")
        print(f"  Summary: {frame['frame_summary']}")
        if "processed_image" in frame:
            img_info = frame["processed_image"]
            print(f"  Processed image:")
            print(f"    Format: {img_info.get('format')}")
            print(f"    Encoding: {img_info.get('encoding')}")
            if img_info.get('encoding') == 'file':
                print(f"    Path: {img_info.get('path')}")
                print(f"    Size: {img_info.get('size_bytes')} bytes")
            else:
                print(f"    Base64 length: {len(img_info.get('data', ''))} chars")
            print(f"    Dimensions: {img_info.get('dimensions')}")
    
    print("\n=== Final Response ===")
    final_response = {
        "request_id": vpi_request["request_id"],
        "app_id": vpi_request["app_id"],
        "decision": "allow",
        "result": pacc_result,
    }
    
    # Don't print full base64 data if using base64 encoding
    if vpi_request.get("output_format", {}).get("image_encoding") == "base64":
        print("(Base64 image data omitted from display)")
        # Create a copy without base64 data for display
        display_response = json.loads(json.dumps(final_response))
        for frame in display_response.get("result", {}).get("frames", []):
            if "processed_image" in frame and "data" in frame["processed_image"]:
                frame["processed_image"]["data"] = f"<base64 data: {len(frame['processed_image']['data'])} chars>"
        print(json.dumps(display_response, indent=2))
    else:
        print(json.dumps(final_response, indent=2))
    
    print(f"\n✓ Processed images saved to: {pacc_result['processing_metadata']['output_directory']}")

if __name__ == "__main__":
    main()