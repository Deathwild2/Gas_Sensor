#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// --- CONFIGURACIÓN ---
#define TIEMPO_TRANSMISION 7000 
#define BAT_CAL 1   
#define BAT_ESP 0   
#define SAL_MOSF 4 

int Status_Mosf = 0;

// !!! MAC DEL RECEPTOR !!!
uint8_t receptorMAC[] = {0x58, 0x8C, 0x81, 0xAE, 0xAE, 0x88};

// Variables globales
int canalEncontrado = 0;
bool conexionEstablecida = false;
float peso = 0;
// Estructura
typedef struct Mensaje {
    int dato1;
    int dato2;
    int dato3 = 2;
    int dato4;
} Mensaje;

Mensaje myData;
Mensaje dataIn;

// --- CALLBACKS ---
// Usaremos esto para saber si "encontramos" al receptor
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        conexionEstablecida = true; // ¡Lo encontramos!
    }
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *Rxdata, int data_len) {
    memcpy(&dataIn, Rxdata, sizeof(dataIn));
    peso = dataIn.dato1;
    Serial.printf("<< Respuesta: Peso %.2f kg\n", peso);
}

// --- FUNCIÓN DE CAZA DE CANALES ---
void encontrarCanalReceptor() {
    Serial.println("Iniciando búsqueda del Receptor (Canal 1-13)...");
    
    Mensaje ping; // Mensaje vacío solo para probar conexión
    ping.dato4 = 999; 

    // Bucle infinito hasta encontrarlo
    while (!conexionEstablecida) {
        for (int ch = 1; ch <= 13; ch++) {
            // 1. Cambiar canal WiFi del ESP32
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous(false);

            // 2. Registrar el Peer en este nuevo canal
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, receptorMAC, 6);
            peerInfo.channel = ch; 
            peerInfo.encrypt = false;
            
            // Si ya existe, lo modificamos, si no, lo agregamos
            bool exists = esp_now_is_peer_exist(receptorMAC);
            if (exists) {
                esp_now_del_peer(receptorMAC);
            }
            esp_now_add_peer(&peerInfo);

            // 3. Intentar enviar un paquete
            Serial.printf("Probando Canal %d... ", ch);
            
            // Forzamos la bandera a false antes de enviar
            conexionEstablecida = false; 
            
            esp_err_t result = esp_now_send(receptorMAC, (uint8_t *)&ping, sizeof(ping));
            
            // Esperamos un momento para dar tiempo al Callback OnDataSent de ejecutarse
            delay(100);

            // 4. Verificamos si el Callback puso la bandera en true
            if (conexionEstablecida) {
                canalEncontrado = ch;
                Serial.printf("\n¡EXITO! Receptor encontrado en Canal %d\n", canalEncontrado);
                return; // Salimos de la función, ya estamos conectados
            } else {
                Serial.println("Sin respuesta.");
            }
        }
        Serial.println("Barrido completo sin éxito. Reintentando...");
        delay(1000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // Configurar en modo Estación (pero sin conectar a nada)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) ESP.restart();

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    pinMode(BAT_CAL, INPUT);
    pinMode(BAT_ESP, INPUT);
    pinMode(SAL_MOSF, OUTPUT);

    // --- AQUÍ OCURRE LA MAGIA ---
    // El código se quedará aquí hasta encontrar al receptor
    encontrarCanalReceptor();
}

void loop() {
    
    // Cálculos
    myData.dato1 = (analogRead(BAT_CAL) - 1400) * (100 / 850.0);


    int nivel = (analogRead(BAT_ESP)*3)/4095.0;           //pasamos a voltaje analogico 
    nivel = ((100*(((nivel)*3)-6))/3.3);  
    if (nivel <0){
      nivel = 0;    
    }
    myData.dato2 = nivel;
  
    if ((peso > 5) && (myData.dato1 > 60)) Status_Mosf = 0; 
    else Status_Mosf = 1;
    
    myData.dato3 = Status_Mosf;
    myData.dato4 = 4;

    digitalWrite(SAL_MOSF, Status_Mosf);   
    delay(10);

    // --- CORRECCIÓN AQUÍ ---
    // Usamos %d para indicar dónde va el número y \n para saltar de línea
    // También cambiamos BAT_ESP por myData.dato2 (el valor real calculado)
    Serial.printf("Bateria del esp: %d %%\n", myData.dato2);      
    Serial.printf("Bateria del calefont: %d %%\n", myData.dato1);
    Serial.printf("Status del mosfet: %d\n", Status_Mosf);
    Serial.printf("Peso: %.2f %%\n", peso);
    Serial.println("-----------------------"); // Separador visual
    
    esp_now_send(receptorMAC, (uint8_t *)&myData, sizeof(myData));

    delay(TIEMPO_TRANSMISION);
}
