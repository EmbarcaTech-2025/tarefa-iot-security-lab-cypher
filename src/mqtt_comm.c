#include "lwip/apps/mqtt.h"       // Biblioteca MQTT do lwIP
#include "include/mqtt_comm.h"    // Header file com as declarações locais
// Base: https://github.com/BitDogLab/BitDogLab-C/blob/main/wifi_button_and_led/lwipopts.h
#include "lwipopts.h"             // Configurações customizadas do lwIP
#include <string.h>                 // Para funções de string como strlen()
#include "pico/stdlib.h" 

/* Variável global estática para armazenar a instância do cliente MQTT
 * 'static' limita o escopo deste arquivo */
static mqtt_client_t *client;

uint32_t ultima_timestamp_recebida = 0; // Usando uint32_t para maior clareza

#define MAX_MQTT_TOPIC_SIZE 128 // Tamanho máximo (em bytes) para armazenar o nome do tópico recebido
#define MAX_MQTT_PAYLOAD_SIZE 512 // Tamanho máximo (em bytes) para armazenar a mensagem recebida
static char incoming_topic_buf[MAX_MQTT_TOPIC_SIZE];     // Buffer para o nome do tópico da mensagem recebida
static char incoming_payload_buf[MAX_MQTT_PAYLOAD_SIZE]; // Buffer para o conteúdo da mensagem recebida (payload)
static uint32_t incoming_payload_len;                    // Tamanho da mensagem recebida
static uint8_t new_message_received = 0;                 // Flag para indicar se uma nova mensagem foi recebida por completo


/* Callback de início de recepção de publicação MQTT
 * Chamado quando o cabeçalho de uma nova publicação é recebido (antes dos dados).
 *
 * Parâmetros:
 *   - arg: argumento opcional (não utilizado)
 *   - topic: nome do tópico associado à mensagem recebida
 *   - tot_len: tamanho total da mensagem
 */
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    LWIP_UNUSED_ARG(arg);  // Ignora o argumento não utilizado

    printf("MQTT RX Publish: Tópico '%s', Tamanho Total do Payload: %lu bytes\n", topic, tot_len);

    // Verifica se o tópico recebido cabe no buffer
    if (strlen(topic) >= MAX_MQTT_TOPIC_SIZE) {
        printf("MQTT RX Publish: Tópico muito longo (%zu vs %d max)\n", strlen(topic), MAX_MQTT_TOPIC_SIZE - 1);
        new_message_received = 0;  // Marca que a mensagem não será processada
        return;
    }

    // Copia o tópico para o buffer interno com terminação segura
    strncpy(incoming_topic_buf, topic, MAX_MQTT_TOPIC_SIZE - 1);
    incoming_topic_buf[MAX_MQTT_TOPIC_SIZE - 1] = '\0';

    // Verifica se o payload será maior que o buffer disponível
    if (tot_len >= MAX_MQTT_PAYLOAD_SIZE) {
        printf("MQTT RX Publish: Payload muito grande (%lu bytes vs %d max). Descartando.\n", tot_len, MAX_MQTT_PAYLOAD_SIZE - 1);
        incoming_payload_len = 0;
        new_message_received = 0;
        return;
    }

    // Inicializa buffers para recepção dos dados da nova mensagem
    incoming_payload_len = 0;
    new_message_received = 0;
}

void on_message(char* topic, char* msg) {
    // 1. Parse do JSON (exemplo simplificado)
    uint32_t nova_timestamp;
    float valor;
    if (sscanf(msg, "{\"valor\":%f,\"ts\":%lu}", &valor, &nova_timestamp) != 2) {
        printf("Erro no parse da mensagem!\n");
        return;
    }

    // 2. Verificação de replay
    if (nova_timestamp > ultima_timestamp_recebida) {
        ultima_timestamp_recebida = nova_timestamp;
        printf("Nova leitura: %.2f (ts: %lu)\n", valor, nova_timestamp);
        
        // --> Processar dados aqui <--
        
    } else {
        printf("Replay detectado (ts: %lu <= %lu)\n", nova_timestamp, ultima_timestamp_recebida);
    }
}

/* Callback de recepção de dados MQTT
 * Chamado sempre que um fragmento de payload de uma publicação MQTT é recebido.
 * 
 * Parâmetros:
 *   - arg: argumento opcional (não utilizado)
 *   - data: ponteiro para o fragmento de dados recebido
 *   - len: tamanho do fragmento recebido (em bytes)
 *   - flags: flags que indicam se o fragmento é o último da mensagem (MQTT_DATA_FLAG_LAST)
 */
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    LWIP_UNUSED_ARG(arg);  // Ignora o argumento não utilizado

    // Verifica se ao adicionar o novo fragmento haverá estouro do buffer de payload
    if (incoming_payload_len + len > MAX_MQTT_PAYLOAD_SIZE - 1) {
        printf("MQTT RX Data: Buffer de payload excedido ao concatenar. Descartando fragmento.\n");
        return;
    }

    // Copia os dados recebidos para o buffer, na posição correta
    memcpy(incoming_payload_buf + incoming_payload_len, data, len);
    incoming_payload_len += len;
    incoming_payload_buf[incoming_payload_len] = '\0';  // Garante terminação da string

    // Se este é o último fragmento da mensagem
    if (flags & MQTT_DATA_FLAG_LAST) {
        printf("MQTT RX Data: Mensagem completa recebida (%lu bytes).\n", incoming_payload_len);
        new_message_received = 1;  // Marca que há nova mensagem disponível

        // Chama a função de processamento da mensagem recebida
        on_message(incoming_topic_buf, incoming_payload_buf);

        // Limpa os buffers e flags para próxima mensagem
        incoming_payload_len = 0;
        new_message_received = 0;
        incoming_topic_buf[0] = '\0';
    }
}

/* Callback de conexão MQTT - chamado quando o status da conexão muda
 * Parâmetros:
 *   - client: instância do cliente MQTT
 *   - arg: argumento opcional (não usado aqui)
 *   - status: resultado da tentativa de conexão */
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("Conectado ao broker MQTT com sucesso!\n");
    } else {
        printf("Falha ao conectar ao broker, código: %d\n", status);
    }
}

/* Função para configurar e iniciar a conexão MQTT
 * Parâmetros:
 *   - client_id: identificador único para este cliente
 *   - broker_ip: endereço IP do broker como string (ex: "192.168.1.1")
 *   - user: nome de usuário para autenticação (pode ser NULL)
 *   - pass: senha para autenticação (pode ser NULL) */
void mqtt_setup(const char *client_id, const char *broker_ip, const char *user, const char *pass) {
    ip_addr_t broker_addr;  // Estrutura para armazenar o IP do broker
    
    // Converte o IP de string para formato numérico
    if (!ip4addr_aton(broker_ip, &broker_addr)) {
        printf("Erro no IP\n");
        return;
    }

    // Cria uma nova instância do cliente MQTT
    client = mqtt_client_new();
    if (client == NULL) {
        printf("Falha ao criar o cliente MQTT\n");
        return;
    }

    mqtt_set_inpub_callback(client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, NULL);

    // Configura as informações de conexão do cliente
    struct mqtt_connect_client_info_t ci = {
        .client_id = client_id,  // ID do cliente
        .client_user = user,     // Usuário (opcional)
        .client_pass = pass      // Senha (opcional)
    };

    // Inicia a conexão com o broker
    // Parâmetros:
    //   - client: instância do cliente
    //   - &broker_addr: endereço do broker
    //   - 1883: porta padrão MQTT
    //   - mqtt_connection_cb: callback de status
    //   - NULL: argumento opcional para o callback
    //   - &ci: informações de conexão
    mqtt_client_connect(client, &broker_addr, 1883, mqtt_connection_cb, NULL, &ci);
}

/* Callback de confirmação de publicação
 * Chamado quando o broker confirma recebimento da mensagem (para QoS > 0)
 * Parâmetros:
 *   - arg: argumento opcional
 *   - result: código de resultado da operação */
static void mqtt_pub_request_cb(void *arg, err_t result) {
    if (result == ERR_OK) {
        printf("Publicação MQTT enviada com sucesso!\n");
    } else {
        printf("Erro ao publicar via MQTT: %d\n", result);
    }
}

/* Callback de confirmação de inscrição
 * Chamado quando o broker confirma a inscrição em um tópico (subscribe)
 * Parâmetros:
 *   - arg: argumento opcional
 *   - err: código de resultado da operação
 */
static void mqtt_sub_request_cb(void *arg, err_t err) {
    if (err == ERR_OK) {
        printf("Inscrição no tópico bem-sucedida!\n");
    } else {
        printf("Falha na inscrição, código de erro: %d\n", err);
    }
}

/* Função para publicar dados em um tópico MQTT
 * Parâmetros:
 *   - topic: nome do tópico (ex: "sensor/temperatura")
 *   - data: payload da mensagem (bytes)
 *   - len: tamanho do payload */
void mqtt_comm_publish(const char *topic, const uint8_t *data, size_t len) {
    // Envia a mensagem MQTT
    err_t status = mqtt_publish(
        client,              // Instância do cliente
        topic,               // Tópico de publicação
        data,                // Dados a serem enviados
        len,                 // Tamanho dos dados
        0,                   // QoS 0 (nenhuma confirmação)
        0,                   // Não reter mensagem
        mqtt_pub_request_cb, // Callback de confirmação
        NULL                 // Argumento para o callback
    );

    if (status != ERR_OK) {
        printf("mqtt_publish falhou ao ser enviada: %d\n", status);
    }
}

/* Função para se inscrever em um tópico MQTT
 * Parâmetros:
 *   - topic: nome do tópico (ex: "sensor/temperatura") */
void mqtt_comm_subscribe(const char* topic){
    // Inscreve-se no tópico MQTT
    err_t status = mqtt_subscribe(
        client,              // Instância do cliente
        topic,               // Tópico a ser inscrito
        0,                   // QoS 0 (nenhuma confirmação)
        mqtt_sub_request_cb, // Callback de confirmação
        NULL                 // Argumento para o callback
    );

    if (status != ERR_OK) {
        printf("mqtt_subscribe falhou: %d\n", status);
    }
}



