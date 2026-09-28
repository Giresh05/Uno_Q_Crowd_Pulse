import urllib.request
import urllib.error
import os

def download_file(url, filename):
    print(f"Downloading {filename}...")
    try:
        # Create a request object with a standard User-Agent to avoid blocks
        req = urllib.request.Request(
            url, 
            headers={'User-Agent': 'Mozilla/5.0'}
        )
        with urllib.request.urlopen(req) as response:
            with open(filename, 'wb') as out_file:
                out_file.write(response.read())
        print(f"[SUCCESS] Saved to {filename}")
    except Exception as e:
        print(f"[ERROR] Failed to download {filename}: {e}")

if __name__ == "__main__":
    base_dir = os.path.dirname(__file__)
    
    # Download YAMNet quantized TFLite model
    model_url = "https://tfhub.dev/google/lite-model/yamnet/classification/tflite/1?lite-format=tflite"
    model_path = os.path.join(base_dir, "yamnet.tflite")
    
    # Download YAMNet class map (to translate network output to human words like "Siren", "Speech")
    csv_url = "https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv"
    csv_path = os.path.join(base_dir, "yamnet_class_map.csv")

    download_file(model_url, model_path)
    download_file(csv_url, csv_path)
