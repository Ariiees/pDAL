"""Predefined reduction policies."""

from typing import Dict


def get_default_policies() -> Dict[str, Dict]:
    """
    Get predefined reduction policies.
    
    Returns:
        Dictionary of policy configurations
    """
    return {
        "traffic_monitoring": {
            "description": "Anonymize people, keep vehicles for traffic analysis",
            "reduction_rules": {
                "person": {
                    "action": "anonymize", 
                    "method": "blur", 
                    "output_metadata": False
                },
                "bicycle": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                },
                "car": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "truck": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "bus": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "motorcycle": {
                    "action": "anonymize", 
                    "method": "blur", 
                    "output_metadata": True
                }
            }
        },
        
        "pedestrian_safety": {
            "description": "Keep people visible, anonymize vehicles",
            "reduction_rules": {
                "person": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "bicycle": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "car": {
                    "action": "anonymize", 
                    "method": "silhouette", 
                    "output_metadata": False
                },
                "truck": {
                    "action": "anonymize", 
                    "method": "silhouette", 
                    "output_metadata": False
                },
                "bus": {
                    "action": "anonymize", 
                    "method": "silhouette", 
                    "output_metadata": False
                },
                "motorcycle": {
                    "action": "anonymize", 
                    "method": "silhouette", 
                    "output_metadata": False
                }
            }
        },
        
        "full_anonymization": {
            "description": "Anonymize all detected objects for maximum privacy",
            "reduction_rules": {
                "person": {
                    "action": "anonymize", 
                    "method": "blackout", 
                    "output_metadata": True
                },
                "bicycle": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                },
                "car": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                },
                "truck": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                },
                "bus": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                },
                "motorcycle": {
                    "action": "anonymize", 
                    "method": "pixelate", 
                    "output_metadata": True
                }
            }
        },
        
        "research_mode": {
            "description": "Keep everything visible with full metadata",
            "reduction_rules": {
                "person": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "bicycle": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "car": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "truck": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "bus": {
                    "action": "keep", 
                    "output_metadata": True
                },
                "motorcycle": {
                    "action": "keep", 
                    "output_metadata": True
                }
            }
        }
    }


def get_policy_names() -> list:
    """Get list of available policy names."""
    return list(get_default_policies().keys())


def get_policy_description(policy_id: str) -> str:
    """Get description for a policy."""
    policies = get_default_policies()
    if policy_id in policies:
        return policies[policy_id].get("description", "No description")
    return "Unknown policy"


def create_custom_policy(policy_id: str, rules: Dict) -> Dict:
    """
    Create a custom policy configuration.
    
    Args:
        policy_id: Unique identifier for the policy
        rules: Dictionary mapping class names to rule configs
               Each rule should have: action, method (optional), output_metadata
               
    Returns:
        Policy configuration dictionary
    """
    return {
        "policy_id": policy_id,
        "description": "Custom policy",
        "reduction_rules": rules
    }