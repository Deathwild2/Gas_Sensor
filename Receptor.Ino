#include <esp_now.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // Necesario para HTTPS (Telegram)
#include <HTTPClient.h>
#include <HX711_ADC.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h> 

// --- CONFIGURACIÓN DE PINES Y TIEMPOS ---
unsigned long t = 0;
const int serialPrintInterval = 7000; // Enviar cada 7 segundos
int CHANNEL = 1; 
#define BAT_INT 0 
#define LED_INT 20
float nivel = 0;

// --- TELEGRAM ---
const String botToken = "8582399485:AAH5UT1sH_qlphvMR8RZbDVU8PrCz1zcgPk"; 
const String chatId = "8560149673"; 

// --- MAC DEL EMISOR ---
// Asegúrate que esta sea la MAC correcta de tu OTRO Esp32
uint8_t emisorMAC[] = {0x58, 0x8C, 0x81, 0xAD, 0x34, 0x0C}; 

// --- BÁSCULA (HX711) ---
float calibrationValue = -13.66; // Valor de Calibracion
const int HX711_dout = 6; 
const int HX711_sck = 7;
HX711_ADC LoadCell(HX711_dout, HX711_sck);

// --- ESTRUCTURA DE DATOS  ---
typedef struct Mensaje {
    int dato1; // Batería Calefont
    int dato2; // Batería ESP Emisor
    int dato3; // Status Mosfet
    int dato4; // Extra
} Mensaje;

Mensaje dataRecibida; 
Mensaje dataEnviada;  


// --- FUNCIÓN EXTRA: CODIFICAR URL PARA TELEGRAM ---
// Convierte espacios en %20, saltos de línea en %0A, etc.
String urlEncode(String value) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    char code2;
    for (int i = 0; i < value.length(); i++) {
        c = value.charAt(i);
        if (c == ' ') {
            encodedString += "%20";
        } else if (isalnum(c)) {
            encodedString += c;
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) {
                code1 = (c & 0xf) - 10 + 'A';
            }
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) {
                code0 = c - 10 + 'A';
            }
            code2 = '\0';
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

// --- CALLBACKS ESP-NOW ---
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Opcional: Saber si el emisor recibió nuestra respuesta
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *Rxdata, int data_len) {
    memcpy(&dataRecibida, Rxdata, sizeof(dataRecibida));
    
    Serial.println("\n>>> DATO RECIBIDO DEL EMISOR <<<");
    Serial.printf("Bat Calefont: %d %%\n", dataRecibida.dato1);
    Serial.printf("Bat ESP: %d %%\n", dataRecibida.dato2);
    Serial.printf("Status Mosfet: %d\n", dataRecibida.dato3);
    
}

// --- INICIALIZACIÓN WIFI Y ESP-NOW ---
void wifi_init_con_manager() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); 
    
    // Crea una red llamada "CONFIGURAR_BASCULA" si no conecta
    if(!wm.autoConnect("CONFIGURAR_BASCULA")) {
        Serial.println("Fallo al conectar WiFi. Reiniciando...");
        ESP.restart();
    } 

    // IMPORTANTE: ESP-NOW debe usar el mismo canal que el Router
    CHANNEL = WiFi.channel();
    Serial.print("Conectado a WiFi. Canal: ");
    Serial.println(CHANNEL);

    if (esp_now_init() != ESP_OK) {
        Serial.println("Error inicializando ESP-NOW");
        ESP.restart();
    }

    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb(OnDataSent);

    // Registramos al Emisor como 'Peer'
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, emisorMAC, 6);
    peerInfo.channel = CHANNEL; 
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK){
        Serial.println("Nota: El emisor ya estaba registrado o hubo error.");
    }
}

// --- ENVÍO A TELEGRAM ---
void sendMessageToTelegram(String message) {
    if(WiFi.status() != WL_CONNECTED) {
        Serial.println("Error: WiFi desconectado.");
        return;
    }

    // Intentaremos enviar hasta 3 veces si falla
    bool enviado = false;
    int intentos = 0;

    while (!enviado && intentos < 3) {
        intentos++;
        if (intentos > 1) Serial.printf("Reintentando envío a Telegram (Intento %d)...\n", intentos);

        WiFiClientSecure client;
        client.setInsecure(); // Ignorar certificado
        client.setHandshakeTimeout(10000); // Darle 10 segundos para el "saludo" SSL

        HTTPClient http;
        
        String mensajeCodificado = urlEncode(message);
        String url = "https://api.telegram.org/bot" + botToken + "/sendMessage?chat_id=" + chatId + "&text=" + mensajeCodificado;

        // Iniciar conexión
        if (http.begin(client, url)) {
            int httpCode = http.GET();
            
            if (httpCode > 0) {
                if (httpCode == 200) {
                    Serial.println("Telegram: Mensaje enviado con ÉXITO.");
                    enviado = true; // Salimos del bucle
                } else {
                    Serial.printf("Telegram: Servidor respondió error HTTP %d\n", httpCode);
                    // Si el error es 400 (Bad Request), no tiene sentido reintentar, está mal escrito
                    if (httpCode == 400) break; 
                }
            } else {
                Serial.printf("Telegram: Error de conexión (%s)\n", http.errorToString(httpCode).c_str());
            }
            http.end(); // Cerramos recurso
        } else {
            Serial.println("Telegram: No se pudo iniciar la conexión HTTP.");
        }

        // Si falló, esperamos 1.5 segundos antes de reintentar para dejar enfriar la conexión
        if (!enviado) {
            delay(1500); 
        }
    }

    if (!enviado) {
        Serial.println("Telegram: Fallo definitivo tras 3 intentos.");
        // Imprimimos la calidad de la señal para diagnosticar
        long rssi = WiFi.RSSI();
        Serial.printf("Calidad de señal WiFi (RSSI): %ld dBm (Menos de -80 es malo)\n", rssi);
    }
}

void iniciar_bascula() {
    LoadCell.begin();
    delay(1000);
    LoadCell.start(2000, true);
    if (LoadCell.getTareTimeoutFlag()) {
        Serial.println("ERROR CRÍTICO: No se detecta la celda de carga. Revisa cableado.");
    } else {
        Serial.println("Báscula iniciada y tarada.");
    }
    LoadCell.setCalFactor(calibrationValue);
}

// --- SETUP ---
void setup() {
    delay(2000); // Espera para estabilizar voltaje
    Serial.begin(115200);
    Serial.println("Iniciando sistema...");
    pinMode(BAT_INT, INPUT);
    pinMode(LED_INT, OUTPUT);
    delay(100);
    iniciar_bascula();
    wifi_init_con_manager();
    
}

// --- LOOP ---
void loop() {
    // Mantiene la báscula leyendo datos
    LoadCell.update();

    
    // Temporizador no bloqueante
    if (millis() > t + serialPrintInterval) {

        //Lee la bateria del receptor
        nivel = (analogRead(BAT_INT)*3)/4095.0;           //pasamos a voltaje analogico 
        nivel = ((100*(((nivel)*4)-6))/2.4);              //pasamos a porcentaje
        if (nivel<0){
            nivel = 0;
        }
        
        else if (nivel>100){
            nivel = 100;
        }
        
        float pesoActual = LoadCell.getData()/1000;
        
        // 1. Imprimir en Serial (Corregido)
        Serial.printf("Peso Local: %.2f \n", pesoActual);

        // 2. Enviar respuesta al Emisor por ESP-NOW

        pesoActual = ((pesoActual-75)*100)/85;

        if (pesoActual < 0){
          pesoActual = 0;
        }
        dataEnviada.dato1 = (int)(pesoActual); 
      
        
        esp_now_send(emisorMAC, (uint8_t *) &dataEnviada, sizeof(dataEnviada));

        // 3. Enviar reporte a Telegram
        if (WiFi.status() == WL_CONNECTED) {
             String mensaje = "📊 REPORTE DE ESTADO 📊\n";
             mensaje += "Porcentaje de Gas: " + String(pesoActual, 2) + "%\n";
             mensaje += "Bat. Calefont: " + String(dataRecibida.dato1) + "%\n";
             mensaje += "Bat. Controlador Calefont: " + String(dataRecibida.dato2) + "%\n";
             mensaje += "Bat. Controlador Base: " + String(nivel) + "%\n";
              
              
             Serial.printf("Status Mosfet: %d\n", dataRecibida.dato3);
             Serial.println(pesoActual);

             if (pesoActual > 5 && pesoActual < 15) {
                mensaje += "Gas a punto de agotarse\n"; 
             }
             else if (pesoActual < 5) {
                mensaje += "Reemplazar Cilindro\n"; 
             }
           
             if (dataRecibida.dato3 == 0) {
                  mensaje += "Calefont funcionando normalmente\n"; // Todo en un solo par de comillas
                  digitalWrite(LED_INT, HIGH);
              }
              else {
                  mensaje += "Calefont desconectado\n";
                  digitalWrite(LED_INT, LOW);
              }
             Serial.println("Enviando reporte a Telegram...");
             sendMessageToTelegram(mensaje);
        } else {
            Serial.println("WiFi perdido... intentando reconexión.");
            WiFi.reconnect();
        }
        dataRecibida.dato2 = 0;
        dataRecibida.dato3 = 0;
        
        t = millis(); // Reseteamos el temporizador
    }
}
