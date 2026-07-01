# -*- coding: utf-8 -*-
from flask import Flask, render_template, request, jsonify, redirect, url_for
from supabase import create_client, Client
from datetime import datetime
import zoneinfo

app = Flask(__name__)

SUPABASE_URL = "https://mbnidgyturqfjwcoqhsd.supabase.co"
SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im1ibmlkZ3l0dXJxZmp3Y29xaHNkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODI4NTM2MjMsImV4cCI6MjA5ODQyOTYyM30.UUr6gMG5YNHq2lH-6nV0-YKjrIWSpHmNWOm5NPVMP0c"

try:
    supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)
except Exception:
    pass

# Los tres estados operativos del comando global: "RUN", "EMERGENCY_STOP", "RESET_REQ"
system_command = "RUN" 

@app.template_filter('to_local_time')
def to_local_time_filter(utc_string):
    try:
        clean_string = utc_string.split('.')[0].replace('Z', '')
        if '+' in clean_string:
            clean_string = clean_string.split('+')[0]
        utc_dt = datetime.strptime(clean_string, "%Y-%m-%dT%H:%M:%S")
        utc_dt = utc_dt.replace(tzinfo=zoneinfo.ZoneInfo("UTC"))
        local_dt = utc_dt.astimezone(zoneinfo.ZoneInfo("America/Santiago"))
        return local_dt.strftime("%H:%M:%S — %d/%m/%Y")
    except Exception:
        return utc_string

@app.route('/api/log', methods=['POST'])
def log_event():
    global system_command
    if not request.is_json:
        return jsonify({"status": "error", "message": "Invalid Content-Type"}), 400
    
    data = request.json
    received_event = data.get("event_type", "UNKNOWN")
    
    # CONTROL DE FLUJO MAESTRO:
    if received_event == "SYSTEM_RESET":
        # Si el hardware confirma que proceso el rearme, cerramos el ciclo volviendo a "RUN"
        system_command = "RUN"
    elif received_event == "CRITICAL" and system_command != "RESET_REQ":
        # Si entra un fallo critico (y no estamos rearmando), bloqueamos el servidor
        system_command = "EMERGENCY_STOP"

    try:
        supabase.table("machine_logs").insert({
            "event_type": received_event,
            "x_axis": float(data.get("x_axis", 0.0)),
            "y_axis": float(data.get("y_axis", 0.0)),
            "z_axis": float(data.get("z_axis", 0.0)),
            "description": data.get("description", "No specification")
        }).execute()
    except Exception:
        return jsonify({"command": system_command, "db_status": "database_error"}), 200
    
    return jsonify({"command": system_command, "db_status": "success"}), 200

@app.route('/command', methods=['POST'])
def update_command():
    global system_command
    action = request.form.get("action")
    if action == "RESET":
        system_command = "RESET_REQ"
    elif action == "STOP":
        system_command = "EMERGENCY_STOP"
    return redirect(url_for('dashboard'))

@app.route('/')
def dashboard():
    logs_data = get_logs()
    current_status = "OPERATIONAL"
    
    if system_command == "EMERGENCY_STOP":
        current_status = "REMOTE_STOP_ACTIVE"
        if logs_data and logs_data[0].get("event_type") == "CRITICAL":
            current_status = "CRITICAL_G_FORCE"
    elif system_command == "RESET_REQ":
        current_status = "RESET_PENDING"
    elif logs_data and logs_data[0].get("event_type") == "WARNING":
        current_status = "MACHINE_MISALIGNMENT"
        
    return render_template('index.html', logs=logs_data, cmd=system_command, status=current_status)

def get_logs():
    try:
        response = supabase.table("machine_logs").select("*").order("created_at", desc=True).limit(10).execute()
        return response.data
    except Exception:
        return []

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
