/**
 * Sistema de Monitoreo de Telemetría Estructural y Control de Enclavamientos
 * Módulo: Firmware del Nodo de Hardware Inalámbrico (ESP32)
 * Estándar de Documentación: Especificaciones de Ingeniería Industrial (Chile)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ADXL345_WE.h> 
#include <ArduinoJson.h> 

// Parámetros de red e infraestructura (Datos sensibles anonimizados)
const char* ssid      = "NOMBRE_DE_TU_RED_WIFI";
const char* password  = "CLAVE_DE_TU_RED_WIFI";
const char* serverUrl = "http://IP_DE_TU_SERVIDOR_FLASK:5000/api/log"; 

// Asignación de Pines para Indicación Lumínica de Torreta Externa (Norma IEC 60204-1)
const int EXT_LED_VERDE    = 12;    
const int EXT_LED_AMARILLO = 14; 
const int EXT_LED_ROJO     = 27;     

// Variables de control de tiempo y filtrado de transmisión (Heartbeat)
unsigned long lastHeartbeatTime = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000; // Intervalo base de 10 segundos
String lastSentEventType = "";

// Banderas lógicas de control de estado del hardware
bool hardwareSafetyLatch = false; // Enclavamiento de seguridad ante fallos
bool flashToggle = false;         // Bandera para la intermitencia del canal crítico

// Instanciación del sensor acelerómetro mediante dirección base I2C
ADXL345_WE myAcc = ADXL345_WE(0x53);

/**
 * Control centralizado para los indicadores LED internos de la placa.
 * Aplica lógica invertida (Active-LOW: false = HIGH/Apagado, true = LOW/Encendido).
 */
void setOnboardLED(bool red, bool green, bool blue) {
  digitalWrite(LED_RED, red ? LOW : HIGH);
  digitalWrite(LED_GREEN, green ? LOW : HIGH);
  digitalWrite(LED_BLUE, blue ? LOW : HIGH);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  
  // Configuración de direcciones de canales de salida digital
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  
  pinMode(EXT_LED_VERDE, OUTPUT);
  pinMode(EXT_LED_AMARILLO, OUTPUT);
  pinMode(EXT_LED_ROJO, OUTPUT);

  // Inicialización de indicadores en estado apagado
  setOnboardLED(false, false, false); 

  // Inicialización del enlace de red inalámbrico
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  Serial.println("\nConexión de red establecida.");

  // Inicialización y verificación del bus del transductor I2C
  if(!myAcc.init()){
    Serial.println("Error crítico de comunicación I2C.");
    while(1);
  }
  
  // Configuración de rangos dinámicos según requerimientos estructurales
  myAcc.setDataRate(ADXL345_DATA_RATE_100);
  myAcc.setRange(ADXL345_RANGE_4G);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    // Adquisición de magnitudes vectoriales tridimensionales
    xyzFloat g; 
    myAcc.getGValues(&g); 
    
    // Cálculo de la magnitud del vector resultante (Fuerza G Total)
    float totalG = sqrt(pow(g.x, 2) + pow(g.y, 2) + pow(g.z, 2));
    
    String eventType = "NORMAL";
    String description = "Condiciones operacionales nominativas estables.";

    // 1. Evaluación de Umbrales Críticos e Interbloqueo Mecánico
    if (totalG > 1.40 || totalG < 0.60) {
      hardwareSafetyLatch = true; // Activación del enclavamiento de seguridad
    }

    // 2. Máquina de Estados Lógica e Indicadores Visuales Industriales
    if (hardwareSafetyLatch) {
      eventType = "CRITICAL";
      description = "FALLO CRÍTICO: Sistema bloqueado por exceso de fuerza G.";
      
      // Lógica de intermitencia temporal para el estado crítico de emergencia
      flashToggle = !flashToggle;
      if (flashToggle) {
        setOnboardLED(true, false, false); // Rojo interno activado
        digitalWrite(EXT_LED_ROJO, HIGH);  // Torreta externa activada
      } else {
        setOnboardLED(false, false, false);
        digitalWrite(EXT_LED_ROJO, LOW);  
      }
      digitalWrite(EXT_LED_VERDE, LOW); 
      digitalWrite(EXT_LED_AMARILLO, LOW);
    }
    // Condición de Advertencia: Desalineación o desviación angular excesiva
    else if (abs(g.x) > 0.22 || abs(g.y) > 0.22) {
      eventType = "WARNING";
      description = "ADVERTENCIA: Desalineación estructural fuera de rango.";
      
      setOnboardLED(true, true, false); // Rojo + Verde = Mezcla Amarillo Industrial
      digitalWrite(EXT_LED_VERDE, LOW); 
      digitalWrite(EXT_LED_AMARILLO, HIGH); 
      digitalWrite(EXT_LED_ROJO, LOW);
    } 
    // Condición de Operación Normal y Segura
    else {
      setOnboardLED(false, true, false); // Canal verde exclusivo activo
      digitalWrite(EXT_LED_VERDE, HIGH); 
      digitalWrite(EXT_LED_AMARILLO, LOW); 
      digitalWrite(EXT_LED_ROJO, LOW);
    }

    // 3. Estrategia de Envío Eficiente de Datos (Transmisión por Evento o Heartbeat)
    unsigned long currentTime = millis();
    bool mustSendToCloud = false;

    if (eventType != lastSentEventType) {
      mustSendToCloud = true; // Prioridad alta: Cambio de estado inmediato
    }
    else if (currentTime - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
      mustSendToCloud = true; // Mantención: Reporte de vida periódico
    }

    if (mustSendToCloud) {
      lastHeartbeatTime = currentTime;
      lastSentEventType = eventType;

      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/json");

      // Serialización estructurada del mensaje en formato JSON
      StaticJsonDocument<250> doc;
      doc["event_type"] = eventType;
      doc["x_axis"] = g.x;
      doc["y_axis"] = g.y;
      doc["z_axis"] = g.z;
      doc["description"] = description;

      String jsonString;
      serializeJson(doc, jsonString);

      // Despacho del payload mediante petición POST
      int httpResponseCode = http.POST(jsonString);

      if (httpResponseCode > 0) {
        String responseBody = http.getString();
        StaticJsonDocument<200> resDoc;
        deserializeJson(resDoc, responseBody);
        const char* serverCommand = resDoc["command"];
        
        // Evaluación del comando remoto devuelto por el servidor HMI
        if (strcmp(serverCommand, "EMERGENCY_STOP") == 0) {
          hardwareSafetyLatch = true; 
        }
        else if (strcmp(serverCommand, "RESET_REQ") == 0) {
          // PROCESO DE REARME SEGURO (Handshake Bilateral):
          // Se abre un canal de transmisión temporal para notificar el restablecimiento del sistema
          HTTPClient resetHttp;
          resetHttp.begin(serverUrl);
          resetHttp.addHeader("Content-Type", "application/json");
          
          StaticJsonDocument<250> resetDoc;
          resetDoc["event_type"] = "SYSTEM_RESET";
          resetDoc["x_axis"] = g.x;
          resetDoc["y_axis"] = g.y;
          resetDoc["z_axis"] = g.z;
          resetDoc["description"] = "REARME: Sistema restablecido por el operador de planta.";
          
          String resetJson;
          serializeJson(resetDoc, resetJson);
          resetHttp.POST(resetJson);
          resetHttp.end();
          
          // Liberación inmediata de los bloqueos de hardware