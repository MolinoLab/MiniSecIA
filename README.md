# MiniSecIA

Grabadora de bolsillo sobre la **Waveshare ESP32-S3-ePaper-1.54G**. Pulsas un botón, hablas, y la nota de voz se sube a ConvocAUTOrias (`POST /voz`), que la transcribe y crea la tarea, idea, evento, etc. en Nextcloud. La confirmación es **sonora** (pitidos). La pantalla e-Paper solo muestra imágenes decorativas.

**Hardware:** ESP32-S3-PICO, micrófono + altavoz (ES8311), e-Paper 1,54" (4 colores), microSD, Wi‑Fi 2,4 GHz, botones BOOT y PWR.

---

## Manual de uso

### Encendido y apagado

| Acción | Resultado |
|--------|-----------|
| Encender (batería) | Mantén **PWR** hasta que arranque |
| Apagar | **PWR largo** (~2,5 s). Cuatro notas bajando |
| Sueño profundo | Tras ~90 s sin actividad. Lo despiertan BOOT, PWR o el temporizador de la imagen |

Con USB conectado no se apaga del todo: se queda dormido. Con batería baja (&lt; 3,45 V) suenan tres pitidos muy agudos al sincronizar o al encender.

El Wi‑Fi **no se busca solo**. La cola espera en la microSD hasta que pulses **PWR corto** (por ejemplo al volver a casa).

### Grabar una nota

1. **BOOT corto** → un pitido agudo: estás grabando (LED fijo).
2. Habla con claridad. Empieza por la palabra clave si quieres forzar el tipo (`tarea`, `idea`, `evento`, `comprar`, …).
3. **BOOT corto** otra vez → dos notas bajando: el WAV queda en la cola. Si la **última subida fue bien**, intenta subir ahora; si falló (sin red, etc.), solo almacena.
4. **PWR corto** en reposo → Wi‑Fi, sube la cola, baja los audios de escucha y puede sincronizar el manifiesto de imágenes (sin cambiar la de pantalla). Si la subida va bien, las próximas grabaciones volverán a intentarlo solas.

Límites: mínimo ~0,5 s; máximo por defecto 5 minutos. **PWR corto** mientras grabas **cancela** (tres notas bajando).

### Escuchar

Los artículos que pidas en Telegram con `/escucha` llegan como Ogg Opus a `/escucha` en la microSD durante la sincronización.

- **BOOT largo**: reproduce la más reciente, también si ya se oyó (sin Wi‑Fi).
- **BOOT corto** mientras suena: la anterior.
- **PWR corto** mientras suena: la siguiente. Si ya es la última, para.
- Las ya oídas siguen en `/escucha` o en `/escucha/hechos` y se pueden volver a oír.

### Botones (resumen)

| Pulsación | Función |
|-----------|---------|
| **BOOT corto** | Empezar / terminar grabación. Si está sonando una escucha, la anterior |
| **PWR corto** | Grabando: cancelar. Escuchando: la siguiente, o parar si ya es la última. En reposo: sincronizar (cola + audios; imágenes solo descarga, sin cambiar pantalla) |
| **BOOT largo** (~1,5 s) | Reproducir la escucha más reciente |
| **PWR largo** (~2,5 s) | Apagar |

Dormido: **BOOT** despierta y empieza a grabar. **PWR** solo despierta; hay que pulsar **PWR corto** ya despierto para sincronizar.

### Sonidos

| Sonido | Significado |
|--------|-------------|
| Un pitido agudo | Grabando |
| Dos notas bajando | Grabación guardada en cola (sube ahora si la última fue OK; si no, con PWR corto) |
| Tres notas subiendo | Procesado OK (creado en Nextcloud) o sincronización hecha sin audios nuevos |
| Dos pitidos iguales | Recibido pero en *pendientes* (sin palabra clave o no se entendió) |
| Dos notas bajando, más largas | Sin red / servidor caído: el audio sigue en cola |
| Tono grave largo | Error (audio vacío, token, sin SD, nada que escuchar…) |
| Tres notas bajando cortas | Cancelado o demasiado corto |
| Cuatro notas bajando | Apagando |
| Tres pitidos muy agudos | Batería baja |
| Un pitido corto | Empieza a reproducir un audio de `/escucha` |
| Tres notas graves espaciadas (cuenco) | Aviso de meditación / sentirte |
| Tres notas bajando suaves | Fin de los minutos de meditación |

LED fijo = grabando. LED parpadeando = sincronizando.

### Meditación

Desde Telegram: `/meditacion` (estado), `/meditacion 4 10`, `/meditacion cada 3h 5m`,
`/meditacion ventana 8 22`, `/meditacion pausar` / `reanudar`.

La config llega al bolsillo en el próximo **PWR corto**. El dispositivo despierta solo,
suena el tono de meditación, y al cabo de la duración el tono de fin. No hace falta Wi‑Fi
entre avisos (sí para cambiar la programación).

### Pantalla

Solo imágenes decorativas (200×200, 4 colores). Cambian en local cada `imagen_horas` (por defecto 4 h), no al sincronizar contenido. El refresco completo tarda ~20 s y **parpadea**: es normal en e-Paper de color.

### Sin cobertura

Los WAV quedan en `/cola` de la microSD. Tras grabar solo se abre Wi‑Fi si `subida_ok` (última subida correcta). Si no hay red, se marca el fallo y no se vuelve a intentar hasta **PWR corto**, que además baja Opus y puede descargar imágenes.

---

## Primera puesta en marcha

### 1. microSD (FAT32)

En la raíz, un `config.txt` (copia de [`config.txt.ejemplo`](config.txt.ejemplo)):

```text
wifi1_ssid=TuRed24GHz
wifi1_pass=contraseña
wifi2_ssid=TuMovil
wifi2_pass=contraseña

url=https://voz.tudominio.es
token=TOKEN_DEL_DISPOSITIVO
device_id=bolsillo
```

- Solo Wi‑Fi **2,4 GHz** (el ESP32-S3 no usa 5 GHz).
- `url` sin barra final; `device_id` y `token` deben coincidir con `ESP32_TOKENS` del servidor.
- Al arrancar se importa a la memoria interna (NVS) y **se borra** el archivo (lleva secretos).

Carpetas útiles (se crean solas si hace falta):

```text
/cola/          audios pendientes de subir
/escucha/       Ogg Opus bajados (se pueden volver a oír)
/escucha/hechos/ escuchas de antes; también se vuelven a oír
/fallidos/      rechazados por tamaño
/img/fijas/     tus imágenes .bin (el firmware no las borra)
/img/remotas/   descargadas del servidor
```

### 2. Flashear el firmware

Requisitos: [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **≥ 5.5.1**, cable USB-C de datos.

```bash
cd MiniSecIA
idf.py set-target esp32s3    # solo la primera vez
idf.py build
idf.py -p COM9 flash monitor # en Linux/macOS: /dev/ttyACM0 o similar
```

Si el flasheo falla: mantén **BOOT**, pulsa RESET (o USB con BOOT pulsado), suelta BOOT y repite.

### 3. Comprobar arranque

En el monitor serie deberías ver algo como:

```text
microSD montada
device_id=bolsillo url=https://… redes=2 token=si
Sync bits=… img=1
Conectado a …
Imágenes sincronizadas: …
Mostrando /sdcard/img/remotas/….bin
```

Luego prueba una grabación corta («tarea probar MiniSecIA»).

---

## Configuración (`config.txt` / NVS)

| Clave | Por defecto | Notas |
|-------|-------------|--------|
| `wifi1_ssid` … `wifi3_pass` | (vacío) | Hasta 3 redes 2,4 GHz; gana la de mejor señal |
| `url` | (vacío) | Base HTTPS del servidor, sin `/` final |
| `token` | (vacío) | Bearer del dispositivo |
| `device_id` | `bolsillo` | Clave en `ESP32_TOKENS` |
| `volumen` | 70 | 0–100 |
| `ganancia_mic` | 30 | dB, 0–42 |
| `max_grabacion_seg` | 300 | 10–420 |
| `reposo_seg` | 90 | Segundos hasta sueño profundo |
| `reintento_min` | 15 | Reservado; el Wi‑Fi ya no se reintenta solo |
| `imagen_horas` | 4 | Horas entre cambios de imagen; 0 = solo manual |
| `sync_imagenes_horas` | 24 | Periodo de descarga de imágenes |
| `timeout_http_seg` | 240 | Timeout de `POST /voz` |

También se pueden fijar valores por defecto en `idf.py menuconfig` → **MiniSecIA** (no pongas tokens reales si vas a compartir el binario).

---

## Comunicación con el servidor

- `POST /voz` — multipart WAV + `Authorization: Bearer …`, `X-Request-Id`, `grabado_en`
- `GET /audios/manifest` y `GET /audios/{id}.opus`, `POST /audios/{id}/ack`
- `GET /imagenes/manifest?formato=txt` y `GET /imagenes/{id}.bin`
- `GET /meditacion/config` — manifiesto de alertas de descanso (Telegram `/meditacion`)

| Respuesta | Qué hace el dispositivo |
|-----------|-------------------------|
| 200 `ok` / `pendiente` | Borra el WAV |
| 422 `vacio` | Borra (sin voz) |
| 413 | Mueve a `/fallidos` |
| 400 / 401 / 403 | Para la cola hasta reiniciar (config/token) |
| 409 / 5xx / sin red | Conserva y reintenta |

El `X-Request-Id` es estable por audio: los reintentos no duplican tareas.

---

## Imágenes decorativas

Formato: 200×200, 4 colores, 10 000 bytes (`.bin`). Conversión típica desde el ecosistema ConvocAUTOrias:

```bash
python -m scripts.convertir_imagenes_epaper fotos/*.jpg --salida mis_bin/ --vista-previa
```

Copia a `/img/fijas/` en la SD, o déjalas en el servidor para que el dispositivo las baje a `/img/remotas/`.

---

## Estructura del código

| Archivo | Responsabilidad |
|---------|-----------------|
| `main/main.c` | Arranque, botones, grabación, cola, reposo |
| `main/placa.h` | Mapa de pines |
| `main/ajustes.c` | `config.txt` → NVS |
| `main/almacen.c` | microSD y cola de WAV |
| `main/audio.c` | ES8311: grabación, tonos y reproducción Ogg Opus |
| `main/escucha.c` | Descarga y cola de audios para oír |
| `main/meditacion.c` | Alertas de descanso (timer + tono) |
| `main/red.c` | Wi‑Fi multi-red y SNTP |
| `main/cliente.c` | HTTPS: subida y descargas |
| `main/imagenes.c` | Rotación y sync de imágenes |
| `main/pantalla.c` | Driver e-Paper |
| `main/reloj.c` | RTC PCF85063 |
| `main/energia.c` | Batería, LED, sueño profundo |
| `main/botones.c` | Pulsación corta/larga |

---

## Solución de problemas

| Síntoma | Qué mirar |
|---------|-----------|
| `token=NO` / sin redes | `config.txt` en la SD; reiniciar tras insertarla |
| 401 en subidas | Token distinto al de `ESP32_TOKENS` (comprueba carácter a carácter) |
| No cambia la imagen | Esperar `imagen_horas` (o despertar por timer); debe haber `.bin` en fijas/remotas |
| Pitidos no suenan | Volumen; amplificador (`PIN_PA`) |
| Micrófono en silencio | Canal I2S en `audio.c` (izquierdo por defecto) |
| No flashea | BOOT+RESET; cable de datos; puerto COM correcto |
| Se despierta en bucle | Pull-ups de GPIO0/GPIO18 en `energia.c` |

---

## Licencia y contexto

Proyecto **MolinoLab**. El firmware habla con la API de voz de ConvocAUTOrias; este repositorio contiene solo el firmware del dispositivo.
