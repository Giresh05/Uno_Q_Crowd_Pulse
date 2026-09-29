import threading
import time
import json
import subprocess
import random
import os
import pandas as pd
import numpy as np
import joblib
from flask import Flask, jsonify, render_template
from flask_cors import CORS

# Application Initialization
app = Flask(__name__)
CORS(app)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Global State and Models
env_scaler = None
env_model = None
network_state = {} 
current_gateway_acoustic_score = 0.0

# Bridge Client for MCU Communication
class BridgeClient:
    def __init__(self):
        self.kv_store = {}
        try:
            # Spawn the official CLI monitor for serial communication
            self.process = subprocess.Popen(
                ['arduino-app-cli', 'monitor'],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )
            print("[BRIDGE] Connected to MCU via arduino-app-cli monitor")
        except Exception as e:
            print(f"[BRIDGE] Warning: Could not start arduino-app-cli. Using mock data. ({e})")
            self.process = None
        
    def loop(self):
        """Runs in a background thread to parse BRIDGE:PUT statements from the Monitor."""
        while True:
            if self.process and self.process.stdout:
                try:
                    line = self.process.stdout.readline().strip()
                    if line.startswith("BRIDGE:PUT:"):
                        payload = line.replace("BRIDGE:PUT:", "", 1)
                        if "=" in payload:
                            key, value = payload.split("=", 1)
                            self.kv_store[key] = value
                except Exception:
                    time.sleep(0.1)
            else:
                time.sleep(1)

    def get(self, key):
        return self.kv_store.get(key, None)

# Initialize our custom CLI bridge
mcu_bridge = BridgeClient()
threading.Thread(target=mcu_bridge.loop, daemon=True).start()


def phase1_and_2_arduino_ingestion_thread():
    """
    PHASE 1 & 2: Ingestion & Environmental ML
    Pulls decentralized state table from the MCU-MPU Bridge.
    """
    global network_state, env_scaler, env_model
    
    try:
        env_scaler = joblib.load(os.path.join(BASE_DIR, 'env_scaler.joblib'))
        env_model = joblib.load(os.path.join(BASE_DIR, 'env_model.joblib'))
        print("[PHASE 2] Loaded Environmental Isolation Forest Model.")
    except Exception as e:
        print(f"[ERROR] Could not load ML models. {e}")

    while True:
        mesh_json = mcu_bridge.get("network_state")
        
        if mesh_json and mcu_bridge.process:
            try:
                nodes_data = json.loads(mesh_json)
                
                for nd in nodes_data:
                    nid = str(nd['node_id'])
                    version = nd['version']
                    
                    if nid not in network_state or version > network_state[nid].get("version", 0):
                        env_score = 0.0
                        if env_scaler and env_model:
                            features = pd.DataFrame([{
                                'bmp_temp': nd['bmp_temp'],
                                'bmp_pres': nd['bmp_pres'],
                                'dht_temp': nd['dht_temp'],
                                'dht_humi': nd['dht_humi'],
                                'radar_density_score': nd['radar_density']
                            }])
                            scaled_f = env_scaler.transform(features)
                            anomaly_score_raw = env_model.decision_function(scaled_f)[0]
                            env_score = 1.0 - (1.0 / (1.0 + np.exp(-anomaly_score_raw * -3.0)))
                        
                        network_state[nid] = {
                            "version": version,
                            "metrics": nd,
                            "env_score": float(env_score),
                            "fused_score": 0.0,
                            "alert_state": False,
                            "last_seen": time.time()
                        }
            except Exception:
                pass

            
        time.sleep(0.05)


try:
    import ai_edge_litert.interpreter as tflite
except ImportError:
    try:
        import tflite_runtime.interpreter as tflite
    except ImportError:
        import tensorflow.lite as tflite

def phase3_acoustic_inference_thread():
    global current_gateway_acoustic_score
    print("[PHASE 3] Acoustic Inference Engine Started. Loading YAMNet...")
    
    try:
        model_path = os.path.join(BASE_DIR, "yamnet.tflite")
        interpreter = tflite.Interpreter(model_path=model_path)
        interpreter.allocate_tensors()
        input_details = interpreter.get_input_details()
        output_details = interpreter.get_output_details()
        
        class_names = []
        import csv
        with open(os.path.join(BASE_DIR, 'yamnet_class_map.csv'), 'r') as f:
            reader = csv.reader(f)
            next(reader)
            for row in reader:
                class_names.append(row[2].lower())
        print("[PHASE 3] YAMNet Loaded Successfully.")
    except Exception as e:
        print(f"[ERROR] Could not load YAMNet: {e}")
        return
        
    threat_keywords = ['scream', 'glass', 'gun', 'explos', 'siren', 'shatter', 'smash', 'emergency', 'alarm']

    try:
        cmd = ['arecord', '-D', 'hw:0,0', '-f', 'S16_LE', '-c', '1', '-r', '16000', '-t', 'raw', '-q']
        audio_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except Exception as e:
        print(f"[ERROR] Could not start ALSA arecord: {e}")
        return

    bytes_to_read = 15600 * 2

    while True:
        try:
            raw_bytes = audio_proc.stdout.read(bytes_to_read)
            if len(raw_bytes) < bytes_to_read:
                time.sleep(1)
                continue

            audio_int16 = np.frombuffer(raw_bytes, dtype=np.int16)
            real_audio = audio_int16.astype(np.float32) / 32768.0
            
            interpreter.set_tensor(input_details[0]['index'], real_audio)
            interpreter.invoke()
            scores = interpreter.get_tensor(output_details[0]['index'])
            
            mean_scores = np.mean(scores, axis=0)
            
            risk = 0.0
            for i, score in enumerate(mean_scores):
                if any(k in class_names[i] for k in threat_keywords):
                    risk = max(risk, float(score))
                    
            current_gateway_acoustic_score = float(risk)

        except Exception as e:
            print(f"[ERROR] Audio inference loop crashed: {e}")
            time.sleep(1)


def phase4_fusion_logic_thread():
    global network_state
    print("[PHASE 4] Decision Fusion Engine Started.")
    
    ENV_WEIGHT = 0.7
    ACOUSTIC_WEIGHT = 0.3
    ALERT_THRESHOLD = 0.70
    
    while True:
        for nid, data in network_state.items():
            env_score = data["env_score"]
            ac_score = current_gateway_acoustic_score
            
            fused = (env_score * ENV_WEIGHT) + (ac_score * ACOUSTIC_WEIGHT)
            
            data["fused_score"] = float(fused)
            data["alert_state"] = bool(fused >= ALERT_THRESHOLD)
            
        time.sleep(1)

# Real-Time Inference Serving (JSON API)
@app.route('/')
def index():
    return render_template('dashboard.html')

@app.route('/api/status', methods=['GET'])
def get_status():
    return jsonify({
        "network": network_state,
        "gateway_acoustic": current_gateway_acoustic_score
    })

if __name__ == '__main__':
    t1 = threading.Thread(target=phase1_and_2_arduino_ingestion_thread, daemon=True)
    t1.start()
    
    t2 = threading.Thread(target=phase3_acoustic_inference_thread, daemon=True)
    t2.start()
    
    t3 = threading.Thread(target=phase4_fusion_logic_thread, daemon=True)
    t3.start()
    
    print("[SYSTEM] All inference engines online. Starting API Server...")
    app.run(host='0.0.0.0', port=5000, debug=False, use_reloader=False)
