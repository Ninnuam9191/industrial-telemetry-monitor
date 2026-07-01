#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ADXL345_WE.h> 
#include <ArduinoJson.h> 

const char* ssid     = "SEB";
const char* password = "password";
const char* serverUrl = "http://192.168.134.63:5000/api/log"; 

const int EXT_LED_VERDE = 12;    
const int EXT_LED_AMARILLO = 14; 
const int EXT_LED_ROJO = 27;     

unsigned long lastHeartbeatTime = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000; 
String lastSentEventType = "";

bool hardwareSafetyLatch = false; 
bool flashToggle = false; 

ADXL345_WE myAcc = ADXL345_WE(0x53);

// Helper para limpiar y setear el color del LED de manera explicita (HIGH = APAGADO)
void setOnboardLED(bool red, bool green, bool blue) {
  digitalWrite(LED_RED, red ? LOW : HIGH);
  digitalWrite(LED_GREEN, green ? LOW : HIGH);
  digitalWrite(LED_BLUE, blue ? LOW : HIGH);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  
  pinMode(EXT_LED_VERDE, OUTPUT);
  pinMode(EXT_LED_AMARILLO, OUTPUT);
  pinMode(EXT_LED_ROJO, OUTPUT);

  setOnboardLED(false, false, false); // Apagar canales internos

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  Serial.println("\nConexion de red establecida.");

  if(!myAcc.init()){
    Serial.println("Error critico de comunicacion I2C.");
    while(1);
  }
  myAcc.setDataRate(ADXL345_DATA_RATE_100);
  myAcc.setRange(ADXL345_RANGE_4G);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    xyzFloat g; 
    myAcc.getGValues(&g); 
    float totalG = sqrt(pow(g.x, 2) + pow(g.y, 2) + pow(g.z, 2));
    
    String eventType = "NORMAL";
    String description = "Condiciones operacionales nominativas estables.";

    // 1. Evaluar umbral de impacto fisico
    if (totalG > 1.40 || totalG < 0.60) {
      hardwareSafetyLatch = true; 
    }

    // 2. Control de Estados de Visualizacion e Indicadores Luminosos
    if (hardwareSafetyLatch) {
      eventType = "CRITICAL";
      description = "FALLO CRITICO: Sistema bloqueado por exceso de fuerza G.";
      
      flashToggle = !flashToggle;
      if (flashToggle) {
        setOnboardLED(true, false, false); // ROJO PURO ENCENDIDO
        digitalWrite(EXT_LED_ROJO, HIGH); 
      } else {
        setOnboardLED(false, false, false); // APAGADO
        digitalWrite(EXT_LED_ROJO, LOW);  
      }
      digitalWrite(EXT_LED_VERDE, LOW); 
      digitalWrite(EXT_LED_AMARILLO, LOW);
    }
    // ADVERTENCIA POR DESALINEACION (Amarillo Puro)
    else if (abs(g.x) > 0.22 || abs(g.y) > 0.22) {
      eventType = "WARNING";
      description = "ADVERTENCIA: Desalineacion estructural fuera de rango.";
      
      setOnboardLED(true, true, false); // ROJO + VERDE = AMARILLO INDUSTRIAL
      digitalWrite(EXT_LED_VERDE, LOW); 
      digitalWrite(EXT_LED_AMARILLO, HIGH); 
      digitalWrite(EXT_LED_ROJO, LOW);
    } 
    // OPERACION SEGURA EN CURSO (Verde Puro)
    else {
      setOnboardLED(false, true, false); // VERDE PURO EXCLUSIVO
      digitalWrite(EXT_LED_VERDE, HIGH); 
      digitalWrite(EXT_LED_AMARILLO, LOW); 
      digitalWrite(EXT_LED_ROJO, LOW);
    }

    // 3. Sistema de Transmision Inalambrica Inteligente
    unsigned long currentTime = millis();
    bool mustSendToCloud = false;

    if (eventType != lastSentEventType) {
      mustSendToCloud = true; 
    }
    else if (currentTime - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
      mustSendToCloud = true; 
    }

    if (mustSendToCloud) {
      lastHeartbeatTime = currentTime;
      lastSentEventType = eventType;

      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/json");

      StaticJsonDocument<250> doc;
      doc["event_type"] = eventType;
      doc["x_axis"] = g.x;
      doc["y_axis"] = g.y;
      doc["z_axis"] = g.z;
      doc["description"] = description;

      String jsonString;
      serializeJson(doc, jsonString);

      int httpResponseCode = http.POST(jsonString);

      if (httpResponseCode > 0) {
        String responseBody = http.getString();
        StaticJsonDocument<200> resDoc;
        deserializeJson(resDoc, responseBody);
        const char* serverCommand = resDoc["command"];
        
        if (strcmp(serverCommand, "EMERGENCY_STOP") == 0) {
          hardwareSafetyLatch = true; 
        }
        else if (strcmp(serverCommand, "RESET_REQ") == 0) {
          // INTERCAMBIO DE MENSAJES DE REARME: Enviamos la confirmacion inmediatamente al servidor
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
          
          // Liberamos las restricciones locales de inmediato
          hardwareSafetyLatch = false;
          lastSentEventType = ""; 
          Serial.println("[OK] Rearme procesado y registrado.");
        }
      }
      http.end();
    }
  }
  delay(200); 
}