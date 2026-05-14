# ECE256-Group4
How to use UART with laptop:
1. Install PuTTY. Open it.
2. Select 'serial'
3. Set port to whatever is being used by the TI board
4. Set the baud rate to 115200
5. Press '1' to toggle the FSM

## Running Webserver
- Python 3.x.x required.
- `python -m venv .env`
- Linux/Mac `source ./.env/bin/activate` or Windows `./.env/bin/activate`
- install dependency `pip install flask pyserial`
- Start webserver `python app.py`
- It should serve at `localhost:8080` 
