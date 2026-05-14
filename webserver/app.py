from flask import Flask, render_template, jsonify, request
import serial
import time

app = Flask(__name__)

SERIAL_PORT = '/dev/ttyACM0' 
BAUD_RATE = 115200

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    time.sleep(2) 
except Exception as e:
    print(f"Serial Error: {e}")
    ser = None

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/send_cmd', methods=['POST'])
def send_cmd():
    char_code = request.json.get('cmd')
    if ser and ser.is_open:
        ser.write(char_code.encode('utf-8'))
        return jsonify({"status": "sent", "cmd": char_code})
    return jsonify({"status": "error", "message": "Serial Disconnected"}), 500

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080, debug=True)
