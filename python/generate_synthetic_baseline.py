import pandas as pd
import numpy as np
import os

def generate_baseline_data(num_samples=1000, output_file='baseline_data.csv'):
    """
    Generates synthetic baseline data representing an empty room,
    based on the real sensor readings provided.
    """
    np.random.seed(42) # For reproducibility
    
    # Base stats from user readings
    # Temp/Hum/Pres typically have small fluctuations in a baseline state
    bmp_temp = np.random.normal(loc=34.70, scale=0.3, size=num_samples)
    bmp_pres = np.random.normal(loc=97334.27, scale=15.0, size=num_samples)
    
    dht_temp = np.random.normal(loc=33.90, scale=0.3, size=num_samples)
    dht_humi = np.random.normal(loc=56.30, scale=1.5, size=num_samples)
    
    # Density score for an empty room ranges widely from ~33k to ~140k
    # We'll use a normal distribution centered around the mean of the provided data (~89k)
    # and clip it so it doesn't go below 0 or artificially too high for a baseline.
    density_mean = 89000
    density_std = 30000
    density_score = np.random.normal(loc=density_mean, scale=density_std, size=num_samples)
    density_score = np.clip(density_score, 20000, 160000).astype(int)
    
    # Activity and Distance in empty room based on user data
    # The sensor sometimes reports 'PRESENT' and '0 cm' even when empty due to noise/reflections
    activity = np.ones(num_samples, dtype=int) # 1 = PRESENT (as per the raw output)
    distance = np.zeros(num_samples, dtype=int) # 0 cm
    
    # Create DataFrame
    df = pd.DataFrame({
        'bmp_temp': bmp_temp,
        'bmp_pres': bmp_pres,
        'dht_temp': dht_temp,
        'dht_humi': dht_humi,
        'radar_activity': activity,
        'radar_distance': distance,
        'radar_density_score': density_score
    })
    
    # Save to CSV
    output_path = os.path.join(os.path.dirname(__file__), output_file)
    df.to_csv(output_path, index=False)
    print(f"Generated {num_samples} baseline samples and saved to {output_path}")
    
    # Print some stats to verify
    print("\nBaseline Data Summary:")
    print(df.describe().round(2))

if __name__ == "__main__":
    generate_baseline_data(num_samples=2000)
