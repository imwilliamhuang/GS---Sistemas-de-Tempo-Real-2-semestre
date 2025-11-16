# GS---Sistemas-de-Tempo-Real-2-semestre

Explicação da implementação
1) Visão geral
O sistema é composto por 3 tarefas FreeRTOS rodando em paralelo no ESP32, que se
comunicam por fila e sincronizam o acesso à whitelist (lista de SSIDs autorizados) por
semáforo (mutex). A cada ciclo, a rede conectada (SSID) é lida e verificada contra a
whitelist. Se o SSID não estiver na lista, o sistema gera alerta imediato: log no Serial e
LED piscando (GPIO2).
2) Tarefas e prioridades
• t_wifi_connect (prioridade 3)
Garante conectividade Wi-Fi. Quando desconectado, executa retry com timeout
(várias chamadas com vTaskDelay), evitando laços apertados e preservando
CPU/energia — isso implementa uma técnica de robustez.
• t_wifi_monitor (prioridade 4)
Lê o SSID atual (WiFi.SSID() no Arduino/ESP32). Monta uma mensagem
wifi_check_t com o SSID e o resultado da checagem de autorização, e envia
para a fila alert_queue. A leitura da whitelist é protegida por mutex para evitar
condições de corrida.
• t_alert_handler (prioridade 5)
Consome a fila. Se authorized == false, gera LOG “ALERTA: rede NAO
AUTORIZADA: 'SSID'” e pisca o LED (GPIO2). Se autorizado, registra “OK: rede
autorizada…”. Por ter a maior prioridade, garante resposta rápida ao evento.
Por que prioridades diferentes?
Para garantir tempo de resposta: o handler (alerta) tem prioridade maior, seguido do
monitor (coleta), e por último o conector (manutenção). Assim o alerta não atrasa mesmo
que a reconexão esteja ocorrendo.
3) Comunicação e sincronização
• Fila (alert_queue)
Conecta produtor (monitor) a consumidor (handler), desacoplando ritmo de
coleta/alerta. O tipo trocado (wifi_check_t) contém ssid e authorized.
• Semáforo (mutex) (wl_mutex)
Protege a whitelist (default_whitelist[] copiada para um buffer mutável).
Sempre que o código lê/atualiza a lista, faz xSemaphoreTake/xSemaphoreGive.
Isso evita leituras inconsistentes caso a lista seja alterada (por exemplo, num modo
de teste).
4) Whitelist (≥5 SSIDs)
A lista de redes autorizadas tem pelo menos 5 entradas (ex.: Wokwi-GUEST, CorpNet-5G,
CorpNet-2G, Guest-VLAN-10, Lab-SSID). O match é por comparação de strings (até 32
chars, strncmp), armazenadas num buffer whitelist[8][33].
5) Robustez
• Timeout/Retry de Wi-Fi (na t_wifi_connect): se desconectar, não entra em
laços apertados. Usa delays progressivos para tentar reconectar sem travar o
sistema.
• WDT real (Task Watchdog): cada tarefa é registrada no watchdog
(esp_task_wdt_add) e o alimenta periodicamente (esp_task_wdt_reset). O
WDT evita travamentos: se uma tarefa “travar” e não resetar o WDT dentro do
tempo, o sistema pode acionar um panic/reset (configurável).
6) Alerta e logs
• LED no GPIO2 pisca 3× em caso de rede não autorizada, servindo como
sinalização imediata.
• Logs:
o OK: “OK: rede autorizada: 'SSID'”.
o ALERTA: “ALERTA: rede NAO AUTORIZADA: 'SSID'”.
7) Testes (o que você imprime)
• Print 1: boot + conexão ao Wokwi-GUEST (mostrar SSID e IP no Serial).
• Print 2: mensagem de OK (rede autorizada).
• Print 3: mensagem de ALERTA (rede não autorizada) + evidência do LED piscando.
No Wokwi, como só existe o AP Wokwi-GUEST, você pode remover esse SSID da whitelist
(ou usar o botão STRICT, se deixou no sketch) para forçar o cenário de alerta.
8) Limitações e variações
• Em Wokwi, a simulação de múltiplos APs é limitada — por isso usamos o modo de
teste (política estrita) para gerar o alerta.
• Em hardware real, a troca de redes ocorre naturalmente (é só conectar a um AP
fora da lista) e o sistema alerta automaticamente.
