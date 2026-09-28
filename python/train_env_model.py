import pandas as pd
import numpy as np
from sklearn.ensemble import IsolationForest
from sklearn.preprocessing import StandardScaler
import joblib
import os

def train_model():
    base_dir = os.path.dirname(__file__)
    data_path = os.path.join(base_dir, 'baseline_data.csv')
    
    # 1. Load data
    print(f"Loading baseline data from {data_path}...")
    df = pd.read_csv(data_path)
    
    # Select relevant features for environmental monitoring
    features = ['bmp_temp', 'bmp_pres', 'dht_temp', 'dht_humi', 'radar_density_score']
    X = df[features]
    
    # 2. Scale the features 
    # StandardScaler normalizes the data so features with large numbers (like pressure) 
    # don't overwhelm features with small numbers (like temp).
    print("Scaling features...")
    scaler = StandardScaler()
    X_scaled = scaler.fit_transform(X)
    
    # 3. Train Isolation Forest
    # contamination=0.01 means we assume 1% of our baseline data might be noise/outliers
    print("Training Isolation Forest model...")
    model = IsolationForest(n_estimators=100, contamination=0.01, random_state=42)
    model.fit(X_scaled)
    
    # 4. Save the model and scaler
    model_path = os.path.join(base_dir, 'env_model.joblib')
    scaler_path = os.path.join(base_dir, 'env_scaler.joblib')
    
    joblib.dump(model, model_path)
    joblib.dump(scaler, scaler_path)
    print(f"\n[SUCCESS] Model saved to {model_path}")
    print(f"[SUCCESS] Scaler saved to {scaler_path}")
    
    # 5. Quick test with an "anomaly" (e.g., room gets hot, humid, and high density from a crowd)
    print("\n--- Testing Model Predictions ---")
    
    # Normal empty room
    normal_sample = pd.DataFrame([{
        'bmp_temp': 34.7, 
        'bmp_pres': 97334.0, 
        'dht_temp': 33.9, 
        'dht_humi': 56.3, 
        'radar_density_score': 85000
    }])
    
    # Crowd anomaly (higher temp, higher humidity, extremely high density)
    crowd_sample = pd.DataFrame([{
        'bmp_temp': 37.5,      # Temp increased
        'bmp_pres': 97340.0,   
        'dht_temp': 36.8,      # Temp increased
        'dht_humi': 75.0,      # Humidity spiked
        'radar_density_score': 350000 # Density spiked off the charts
    }])
    
    for name, sample in [("Normal Room", normal_sample), ("Crowd Anomaly", crowd_sample)]:
        # Must scale the live data using the same scaler we used for training
        sample_scaled = scaler.transform(sample)
        
        # Predict: 1 = normal, -1 = anomaly
        prediction = model.predict(sample_scaled)[0] 
        
        # Score: Lower negative numbers mean it's MORE anomalous
        score = model.score_samples(sample_scaled)[0] 
        
        status = "NORMAL (Safe)" if prediction == 1 else "ANOMALY (Alert!)"
        print(f"Scenario: {name}")
        print(f"  Classification: {status}")
        print(f"  Outlier Score:  {score:.3f}\n")

if __name__ == '__main__':
    train_model()
