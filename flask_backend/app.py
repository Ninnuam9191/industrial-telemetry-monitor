# -*- coding: utf-8 -*-
"""
Sistema de Supervisión HMI y Servidor de Telemetría Estructural
Módulo: Backend de Control y Gestión de Eventos (Flask)
Estándar de Documentación: Especificaciones de Ingeniería Industrial (Chile)
"""

from flask import Flask, render_template, request, jsonify, redirect, url_for
from supabase import create_client, Client
from datetime import datetime
import zoneinfo

# Inicialización de la aplicación Flask para la Interfaz Hombre-Máquina (HMI)
app = Flask(__name__)

# Parámetros de conectividad del clúster de base de datos (Datos sensibles anonimizados)
SUPABASE_URL = "URL_DE_TU_PROYECTO_SUPABASE"
SUPABASE_KEY = "TU_CLAVE_ANON_PUBLIC_SUPABASE"

# Instanciación segura del cliente de base de datos Supabase
try:
    supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)
except Exception:
    pass

# Registro de control de comando global del sistema: "RUN", "EMERGENCY_STOP", "RESET_REQ"
system_command = "RUN" 

@app.template_filter('to_local_time')
def to_local_time_filter(utc_string):
    """
    Filtro de plantilla para la conversión formal de marcas de tiempo UTC
    al huso horario de referencia local (América/Santiago).
    """
    try:
        clean_string = utc_string.split('.')[0].replace('Z', '')
        if '+' in clean_string:
            clean_string = clean_string.split('+')[0]
        
        # Conversión y localización de zona horaria según estándar geográfico chileno
        utc_dt = datetime.strptime(clean_string, "%Y-%m-%dT%H:%M:%S")
        utc_dt = utc_dt.replace(tzinfo=zoneinfo.ZoneInfo("UTC"))
        local_dt = utc_dt.astimezone(zoneinfo.ZoneInfo("America/Santiago"))
        
        return local_dt.strftime("%H:%M:%S — %d/%m/%Y")
    except Exception:
        return utc_string

@app.route('/api/log', methods=['POST'])
def log_event():
    """
    Punto de enlace API HTTP POST para la recepción de telemetría desde el nodo de hardware.
    Evalúa la máquina de estados y realiza el almacenamiento persistente en la nube.
    """
    global system_command
    if not request.is_json:
        return jsonify({"status": "error", "message": "Invalid Content-Type"}), 400
    
    data = request.json
    received_event = data.get("event_type", "UNKNOWN")
    
    # CONTROL DE FLUJO MAESTRO Y ARQUITECTURA DE HANDSHAKE:
    if received_event == "SYSTEM_RESET":
        # Confirmación del hardware: Cierre del ciclo de rearme, volviendo a operación nominal
        system_command = "RUN"
    elif received_event == "CRITICAL" and system_command != "RESET_REQ":
        # Detección de fallo en el hardware: Enclavamiento preventivo del estado del servidor
        system_command = "EMERGENCY_STOP"

    # Inserción asíncrona de variables físicas en la tabla histórica de logs
    try:
        supabase.table("machine_logs").insert({
            "event_type": received_event,
            "x_axis": float(data.get("x_axis", 0.0)),
            "y_axis": float(data.get("y_axis", 0.0)),
            "z_axis": float(data.get("z_axis", 0.0)),
            "description": data.get("description", "No specification")
        }).execute()
    except Exception:
        # En caso de desconexión de la BD, se prioriza la respuesta del comando de seguridad al hardware
        return jsonify({"command": system_command, "db_status": "database_error"}), 200
    
    return jsonify({"command": system_command, "db_status": "success"}), 200

@app.route('/command', methods=['POST'])
def update_command():
    """
    Punto de enlace para el procesamiento de acciones directas desde los botones del panel HMI.
    Actualiza la variable global de comando solicitada por el operador.
    """
    global system_command
    action = request.form.get("action")
    if action == "RESET":
        system_command = "RESET_REQ"
    elif action == "STOP":
        system_command = "EMERGENCY_STOP"
    return redirect(url_for('dashboard'))

@app.route('/')
def dashboard():
    """
    Ruta principal del panel de supervisión HMI. Realiza consultas a la base de datos
    y mapea la lógica combinatoria de estados para renderizar la interfaz de usuario.
    """
    logs_data = get_logs()
    current_status = "OPERATIONAL"
    
    # Matriz combinatoria para la determinación visual del banner de seguridad
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
    """
    Recupera los últimos 10 registros históricos desde el clúster remoto de base de datos
    ordenados de forma cronológica descendente.
    """
    try:
        response = supabase.table("machine_logs").select("*").order("created_at", desc=True).limit(10).execute()
        return response.data
    except Exception:
        return []

if __name__ == '__main__':
    # Inicialización del demonio de red en modo de escucha global para entornos locales
    app.run(host='0.0.0.0', port=5000, debug=True)
