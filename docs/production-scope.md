# Penta Multistream — Alcance de producción v1

**Estado:** propuesta base para cerrar la fase 0  
**Fecha:** 2026-09-09  
**Producto:** plugin de OBS para emitir a varios destinos RTMP desde OBS

## 1. Objetivo del producto

Penta Multistream permite configurar varios destinos de transmisión y comenzar o detener la emisión desde OBS, sin utilizar Facebook Live Producer como parte del flujo normal. El plugin debe tratar cada destino de forma independiente y mostrar al usuario un resultado claro cuando una plataforma acepta, rechaza o pierde la transmisión.

La primera versión prioriza un ciclo de transmisión seguro y predecible sobre la cantidad de integraciones. No se considerará lista para producción una función que pueda dejar transmisiones de Facebook activas, ejecutar dos operaciones simultáneas sobre el mismo destino o exponer credenciales en logs, interfaz o archivos de configuración.

## 2. Alcance incluido en v1

### 2.1 Destinos genéricos

- RTMP y RTMPS configurables por el usuario.
- URL del servidor y stream key separadas cuando el proveedor lo requiera.
- Nombre descriptivo y activación individual de cada destino.
- Crear, editar, eliminar, habilitar/deshabilitar y reordenar destinos.
- Un resultado de conexión independiente por destino.
- La clave de transmisión se considera un secreto: no se muestra en logs ni en mensajes de error.

### 2.2 Facebook

- Facebook Pages administradas por el usuario.
- Timeline personal del usuario.
- Inicio de un Live Video desde OBS mediante Graph API; no se requiere abrir Live Producer.
- Un Live Video por destino de Facebook y por sesión de emisión.
- Título y descripción.
- Privacidad para timeline personal: `PUBLIC`, `ALL_FRIENDS` o `SELF`.
- Sin campo de privacidad para Pages, porque las transmisiones de Page son públicas según el flujo implementado.
- Obtención de `secure_stream_url` y uso de esa URL para la salida de OBS.
- Finalización explícita mediante `end_live_video=true` al detener la emisión.
- Presentación de errores de Graph con código y subcódigo, sin tokens ni secretos.
- Desconexión explícita de Facebook que invalida y elimina el token mantenido por el plugin.

### 2.3 Twitch

- Autenticación OAuth de escritorio mediante Qt WebEngine, sin Client Secret.
- Obtención de la stream key mediante Helix y uso del ingest RTMPS oficial de Twitch.
- Actualización opcional del título del canal antes de iniciar la salida de OBS.
- El token OAuth y la stream key dinámica no se guardan en el perfil, logs ni URLs persistidas.
- Al detener OBS se detiene la salida Twitch; no se crea un objeto Live remoto que requiera finalización adicional.

### 2.4 Autenticación y datos

- Login OAuth realizado en una vista Qt WebEngine embebida en OBS, usando el callback de escritorio de Facebook.
- El plugin no contiene App Secret ni ningún secreto de aplicación.
- El App ID de la versión actual es `1609511030686437`.
- Los permisos solicitados inicialmente son únicamente los cuatro necesarios para los flujos implementados:
  - `pages_show_list`: mostrar la lista de Pages administradas y permitir la selección de una Page.
  - `pages_read_engagement`: requerido por Meta para el flujo de Live Video de una Page y para los metadatos usados por la selección.
  - `pages_manage_posts`: requerido por Meta para crear un Live Video mediante `/{PAGE_ID}/live_videos`; no se usa para crear publicaciones normales.
  - `publish_video`: crear un Live Video en el timeline personal mediante `/me/live_videos`.
- No se solicita `public_profile` explícitamente porque Meta lo otorga automáticamente.
- En v1 Linux, los tokens se mantienen en memoria mientras OBS está activo y se persisten únicamente en Secret Service/libsecret; nunca se escriben en perfiles de OBS, logs, URLs persistidas ni archivos del plugin.
- Para Twitch, restaurar el token OAuth, actualizar opcionalmente el título del canal, obtener la stream key desde Helix y usar el ingest RTMPS de Twitch.
- El token OAuth y la stream key dinámica nunca se guardan en el JSON del perfil, logs ni URLs persistidas.

### 2.5 Interfaz y operación

- Estado visible por destino: inactivo, iniciando, emitiendo, reconectando, deteniendo o error.
- Acciones imposibles durante una operación se deshabilitan o se rechazan con un mensaje explicativo.
- Inicio y detención global desde OBS.
- Mensajes traducibles para español e inglés; los textos de usuario y errores conocidos no deben quedar hardcodeados en inglés.
- Indicadores de progreso para OAuth, carga de Pages, creación del Live Video y finalización.
- El usuario puede identificar qué destino falló sin perder el estado de los demás destinos.

## 3. Fuera de alcance de v1

No forman parte de la primera versión de producción:

- Grupos de Facebook.
- Eventos de Facebook.
- Instagram, YouTube u otras integraciones propietarias no implementadas como destino genérico.
- Programación de transmisiones.
- Chat, comentarios, moderación o reacciones.
- Estadísticas avanzadas, analítica histórica o métricas de audiencia.
- Renovación automática de tokens.
- Almacenamiento seguro de tokens en Windows y macOS; Linux usa Secret Service/libsecret.
- Gestión de múltiples cuentas Facebook simultáneas.
- Recuperación automática de Live Videos huérfanos después de un crash. En v1 se conserva un journal local sin tokens ni IDs remotos secretos y se muestra una advertencia para que el usuario verifique manualmente Facebook antes de iniciar otra transmisión.
- Cambio de escenas, perfiles o configuración de OBS durante una operación activa si puede invalidar el ciclo de vida del destino.
- Instaladores firmados y actualización automática.

## 4. Plataformas objetivo

### Decisión propuesta para la primera release

La primera release pública debe declararse **Linux x86_64**, con la versión de OBS utilizada en la validación actual. Linux es la única plataforma validada de extremo a extremo en este repositorio. Windows y macOS se mantienen como objetivos de compatibilidad posteriores, pero no deben anunciarse como soportados hasta disponer de builds, instalación, pruebas y validación de OBS en cada plataforma.

### Requisitos para declarar una plataforma soportada

- Build reproducible desde un entorno documentado.
- Instalación limpia del plugin y de sus recursos.
- Carga correcta en la versión de OBS soportada.
- Flujo RTMP/RTMPS genérico probado.
- OAuth, Pages/timeline y detención de Facebook probados.
- Verificación de rutas de datos, permisos de archivos y certificados TLS.
- Pruebas de cierre normal, error de red y reinicio de OBS.
- Artefacto distribuible y, cuando corresponda, firma o instrucciones de verificación.

## 5. Modelo operativo de una transmisión

Cada destino debe tener una máquina de estados independiente. El flujo esperado es:

1. El usuario configura y habilita destinos.
2. Al iniciar OBS, el plugin valida la configuración sin mostrar secretos.
3. Para Facebook, restaura el token desde el almacén seguro o solicita autorización, carga la cuenta y las Pages, crea el Live Video y obtiene `secure_stream_url`.
4. OBS inicia la salida hacia la URL correspondiente.
5. El destino pasa a `Streaming` sólo después de que su operación de inicio haya sido aceptada.
6. Si se pierde la conexión, se aplica la política de reconexión de OBS y el destino se muestra como `Reconnecting`.
7. Al detener OBS o el destino, se detiene primero la salida local y después se ejecuta el cierre remoto de Facebook.
8. Toda respuesta asíncrona se valida contra la operación vigente; una respuesta antigua no puede modificar una nueva configuración o transmisión.

Estados mínimos de v1:

- `Stopped`
- `Starting`
- `Streaming`
- `Reconnecting`
- `Stopping`
- `Error`
- `Configuring` cuando el destino está siendo editado y aún no participa en una operación

## 6. Requisitos de seguridad

- No incluir App Secret en el binario, repositorio, instalador ni documentación de configuración local.
- No imprimir access tokens, stream keys, URLs con `access_token`, cabeceras de autorización ni respuestas completas que puedan contenerlos.
- Usar HTTPS para OAuth y Graph API.
- Exigir `rtmps://` para la URL generada por Facebook; no aceptar una URL Facebook degradada a `rtmp://`.
- Cancelar solicitudes de red pendientes al desconectar, cambiar de perfil o cerrar OBS.
- Aplicar timeouts y límites de tamaño a respuestas de red.
- Invalidar operaciones y callbacks al cambiar de cuenta, Page, perfil o sesión.
- La página de eliminación de datos debe describir un mecanismo real antes de declarar la integración lista para producción; una página estática informativa por sí sola no es suficiente para el callback de Meta.
- Antes de iniciar una operación Facebook se registra únicamente el destino, nombre y timestamp en un journal local del perfil. El journal no contiene access tokens, Page tokens, stream keys, URLs RTMPS ni `live_id`.
- Si el proceso termina inesperadamente, el siguiente arranque muestra una advertencia persistente y requiere que el usuario reconecte Facebook y verifique manualmente sus Live Videos. El journal sólo se elimina tras un `end_live_video=true` confirmado o mediante una acción explícita de “Marcar como revisado”.
- Los eventos de cambio de perfil y salida utilizan una barrera de cleanup; si supera su timeout, se conserva el journal y se registra que la finalización remota requiere verificación manual.

## 7. Requisitos externos de Meta

Antes de una publicación general se debe verificar, fuera del código del plugin:

- Política de privacidad pública mediante HTTPS: `https://pentaa555.github.io/penta-multistream-legal/`. La fuente local está actualizada; después de publicarla hay que comprobar que GitHub Pages ya no muestre la versión anterior.
- URL pública de instrucciones de eliminación de datos: `https://pentaa555.github.io/penta-multistream-legal/data-deletion.html`. Meta acepta esta modalidad o una devolución de llamada HTTPS; el plugin todavía no implementa el endpoint callback firmado.
- URL de OAuth válida y registrada: `https://localhost:8765/oauth/facebook/callback` para el flujo OAuth embebido. Es una URI local interceptada por Qt WebEngine, no una URL pública de la política.
- Casos de uso, permisos y explicación de revisión alineados con las funciones realmente incluidas.
- Pruebas con usuarios, Pages y permisos renovados en el entorno de Meta.
- App Review completada para los permisos y productos requeridos.
- Live Mode sólo después de completar la revisión y las verificaciones legales/operativas.
- Confirmación de que la política de Meta vigente no impone requisitos adicionales al momento de enviar la app.

## 8. Criterios de aceptación de v1

### Funcionalidad

- Un usuario puede añadir, editar, eliminar, habilitar/deshabilitar y reordenar un destino genérico sin reiniciar OBS.
- Una emisión puede comenzar desde OBS hacia al menos dos destinos configurados, y cada destino muestra su resultado de forma independiente.
- Una emisión RTMP/RTMPS genérica transmite usando la URL y clave configuradas sin intervención del proveedor.
- Un usuario autenticado puede listar sus Pages, seleccionar una Page y crear un Live Video desde OBS.
- Un usuario puede crear un Live Video en su timeline, elegir privacidad válida y transmitir desde OBS.
- Título y descripción llegan al Live Video creado.
- Al detener normalmente, la salida local se detiene y el Live Video de Facebook se finaliza; no se deja el recurso remoto abierto por una carrera conocida del plugin.

### Estabilidad

- Un doble clic o dos llamadas concurrentes a iniciar no crean dos Live Videos para el mismo destino.
- Editar, eliminar, cambiar de Page o desconectar durante `Starting`/`Stopping` no permite que un callback antiguo modifique el destino nuevo.
- OBS puede cerrarse normalmente sin dejar operaciones de red pendientes ni bloquear el cierre.
- Un error HTTP, Graph, TLS, JSON, timeout o cancelación produce un estado y mensaje accionables, sin crash.
- Un fallo en un destino no impide detener ni diagnosticar los demás destinos.
- Se define y prueba una política observable para Live Videos huérfanos tras crash o pérdida de energía.

### Seguridad y privacidad

- Una búsqueda de logs, configuración y artefactos de diagnóstico no encuentra access tokens ni stream keys.
- El botón de desconexión elimina el token de memoria y del almacén seguro, dejando el estado listo para autenticar otra cuenta.
- El binario no contiene App Secret.
- Las URLs y solicitudes a Facebook no exponen tokens en query strings cuando exista una alternativa segura.
- Facebook sólo utiliza URL de ingestión `rtmps://`.

### Calidad y distribución

- Tests unitarios para configuración, estados, parsing de URL, privacidad, errores y cancelación.
- Tests de integración con un servidor HTTP/Graph simulado para respuestas correctas, errores, timeout y callbacks obsoletos.
- Smoke test manual en OBS para RTMP genérico, Page, timeline, stop normal y cierre de OBS.
- Build limpio y reproducible en la plataforma anunciada.
- CTest, análisis/compilación y comprobación de dependencias ejecutados en CI.
- Paquete instalable con README, versión, licencia, política de privacidad y pasos de soporte.

## 9. Brechas actuales respecto al alcance

Estas brechas se registran para implementación y validación. La primera implementación P0 ya cubre los estados, generaciones, bloqueo, cleanup normal, cancelación, RTMPS, redacción, timeouts y desconexión. La barrera de perfil/salida y el journal de recuperación se incorporaron en esta iteración; todavía requieren smoke tests reales en OBS para considerarse release-ready.

### P0 — implementado; pendiente de validación de release

1. Añadir `Starting` y `Stopping` a la máquina de estados.
2. Añadir un operation ID/generation para invalidar callbacks obsoletos.
3. Impedir doble inicio y bloquear edición, borrado, cambio de Page y desconexión durante operaciones incompatibles.
4. Implementar cleanup ordenado: detener salida de OBS y después ejecutar `end_live_video=true`.
5. Aplicar cleanup en cambio de perfil y salida normal de OBS.
6. Cancelar `QNetworkReply` y limpiar callbacks al cerrar, desconectar o cambiar de destino.
7. Definir y probar recuperación de Live Videos huérfanos.
8. Exigir y validar `rtmps://` en URLs de Facebook.
9. Eliminar tokens de query strings y garantizar redacción en logs/errores.
10. Añadir timeouts, manejo de cancelación y clasificación consistente de errores de red, Graph y JSON.
11. Mantener probado el botón de desconexión de Facebook y alinear la política legal con su comportamiento.

### P1 — necesarios para una release operable

1. Paginación de `/me/accounts` y carga robusta de Pages.
2. Validación de errores en `save()` y en operaciones de configuración.
3. `schema_version` y migraciones explícitas para la configuración persistida.
4. Traducción de la UI, errores y estados a español e inglés.
5. Validación de privacidad y mensajes accionables para permisos/restricciones de Meta.
6. Pruebas de Graph/OAuth, UI, OBS y concurrencia.
7. CI, matriz de compilación y paquete reproducible para Linux.
8. Documentación de instalación, permisos Meta, limitaciones y diagnóstico.
9. Revisión final de la página de eliminación de datos y del flujo de soporte.

### P2 — posteriores a la primera release

1. Windows y macOS con builds e instaladores.
2. OAuth con callback automático local, manteniendo el requisito de no usar App Secret.
3. Almacenamiento seguro de tokens para Windows y macOS.
4. Renovación automática de tokens, si Meta y el modelo de seguridad lo permiten.
5. Estadísticas, chat, programación e integraciones adicionales.

## 10. Decisiones pendientes que deben cerrarse

| Decisión | Recomendación inicial | Estado |
|---|---|---|
| OAuth manual vs callback automático | Mantener vista Qt WebEngine con callback localhost interceptado automáticamente | Implementado |
| Tokens en memoria vs almacenamiento seguro | Persistir sólo en Secret Service/libsecret en Linux; nunca en perfiles OBS | Implementado en Linux |
| Plataformas iniciales | Declarar Linux x86_64; no anunciar Windows/macOS sin validación | Propuesta |
| Política tras crash | Marcar estado desconocido, no crear duplicados y documentar recuperación manual/verificación remota | Pendiente de diseño P0 |
| RTMPS para Facebook | Exigirlo siempre | Propuesta firme |
| Múltiples emisiones simultáneas | Un Live Video por destino habilitado; bloquear una segunda operación del mismo destino | Propuesta firme |
| Release Meta | No activar Live Mode ni publicar ampliamente hasta completar App Review y pruebas | Propuesta firme |

## 11. Primera tarea de implementación después de cerrar fase 0

**Diseñar e implementar la máquina de estados y el ciclo de vida de cada destino.**

Orden concreto:

1. Identificar el estado y la generación de cada destino.
2. Introducir `Starting` y `Stopping` y definir las transiciones válidas.
3. Rechazar transiciones duplicadas o imposibles sin lanzar operaciones Graph/RTMP adicionales.
4. Asociar cada solicitud y callback a la generación vigente.
5. Implementar la secuencia de stop y cleanup normal.
6. Añadir tests de transición, doble inicio, stop durante start y callback obsoleto.

No se debe empezar por nuevas plataformas o nuevas funciones de Facebook. La prioridad es que el flujo existente no pueda producir transmisiones duplicadas, recursos remotos huérfanos por una carrera conocida o cambios de estado causados por respuestas antiguas.

## 12. Estado de pruebas Graph/OAuth

La primera capa automatizada de Graph/OAuth quedó incorporada sin realizar llamadas a Facebook:

- `facebook-utils-tests` cubre el callback OAuth HTTPS registrado (`localhost:8765/oauth/facebook/callback`), acepta también el callback de escritorio alternativo, rechaza esquema/host/puerto/ruta no válidos, valida state y token, procesa `error_description`, errores Graph con `code` y `error_subcode`, JSON inválido, normalización `UNLISTED` → `ALL_FRIENDS` y rechazo de URLs `rtmp://`.
- `facebook-provider-tests` levanta un servidor HTTP local y cubre `GET /me` para identificar la cuenta conectada, `GET /me/accounts`, filtrado de Pages sin token, cabecera `Authorization`, creación de Live Video, privacidad, uso de `secure_stream_url`, rechazo de `stream_url` insegura, parada con `end_live_video=true`, error HTTP/Graph 429, timeout y cancelación.
- `destination-tests` mantiene la cobertura del modelo, estados `Starting`/`Stopping` y generaciones de operación.
- `destination-manager-tests` cubre schema_version, migraciones legacy, configuración inválida y rollback de persistencia.
- La suite se ejecuta con `ctest --test-dir build-plugin --output-on-failure`; actualmente pasan los 4 tests.

Estos tests prueban el comportamiento de red contra un servidor local y no prueban la autorización real de Meta. Antes de declarar release-ready todavía requieren prueba manual:

1. OAuth completo en navegador con una app Meta, permisos aprobados y una Page real.
2. Creación y transmisión real desde OBS a una Page y al timeline, usando únicamente la URL RTMPS devuelta por Graph.
3. Detención normal, errores de permisos/token expirado, pérdida de red y comportamiento de OBS durante reconexión.
4. Cambio de perfil, desconexión y cierre de OBS mientras una creación o parada está pendiente; debe verificarse la barrera de cleanup y el journal.
5. Recuperación posterior a crash o pérdida de energía, comprobando manualmente que no se cree un Live duplicado.
6. Validación visual de la UI, traducciones, foco, mensajes de error y carga del binario instalado en la versión objetivo de OBS.
7. Paginación de `/me/accounts`, compatibilidad TLS/certificados y pruebas de instalación limpia en la plataforma anunciada.

## 13. Actualización P1: Pages y errores Graph

Se completó el siguiente bloque P1:

- `/me/accounts` sigue `paging.next` hasta completar la lista de Pages.
- El token sólo se envía a URLs de paginación que coinciden con el esquema, host y puerto del Graph configurado; una URL externa o relativa se rechaza.
- Las Pages se acumulan y sólo se publican en la sesión cuando la paginación termina correctamente.
- Los errores se clasifican como autenticación, permisos, rate limit, indisponibilidad temporal, red, respuesta inválida o desconocido.
- Los mensajes de autenticación, permisos, rate limit y disponibilidad temporal incluyen una acción recomendada sin exponer tokens.
- El código Graph y `error_subcode` se conservan en el mensaje técnico.
- El servidor Graph local cubre dos páginas, 401, 403, 429, 500, JSON inválido, timeout y cancelación.

La suite sigue pasando completa con 4 tests CTest. Permanecen fuera de este bloque los reintentos con backoff para POST y la prueba de carga del módulo dentro de OBS en una instalación limpia.

## 14. Política de reintentos Graph

Se implementaron reintentos limitados únicamente para `GET /me/accounts`:

- Máximo predeterminado: 2 reintentos adicionales.
- Backoff exponencial predeterminado: 250 ms, 500 ms.
- Se reintentan únicamente rate limit, errores temporales 5xx y errores de red clasificados como retryable.
- Si la generación de sesión cambia durante el backoff, el intento pendiente se cancela y el callback recibe cancelación.
- Los límites y el backoff son configurables para tests, pero no se persisten.

`create_live` no se reintenta automáticamente. Un timeout o una conexión perdida después de enviar el POST no permite saber si Meta creó el Live; repetirlo podría generar dos transmisiones. `stop_live` tampoco se reintenta automáticamente hasta definir una comprobación idempotente del estado remoto. En esos casos se muestra el error clasificado y el usuario puede decidir la acción.

Los tests locales cubren recuperación después de 429, agotamiento del límite ante 500 y confirman que cada error de `create_live` produce un solo POST.

## 15. Estado P1: persistencia robusta

Se implementó la primera versión versionada de la persistencia:

- El archivo principal y el journal de recuperación escriben `schema_version: 1`.
- Los perfiles legacy sin versión se leen como versión 0 usando los defaults existentes.
- Las versiones futuras se rechazan sin reemplazar la configuración cargada previamente.
- `load()` devuelve un `LoadStatus` para distinguir perfil ausente, archivo ausente, archivo inválido y versión no soportada.
- Las mutaciones de destinos hacen rollback si falla el guardado: agregar, editar, eliminar, habilitar/deshabilitar y reordenar.
- Los avisos de recuperación también hacen rollback si falla su persistencia.
- Se añadieron hooks de persistencia para tests deterministas sin depender de permisos del sistema.
- La UI muestra errores de lectura, versión no soportada y guardado fallido.

La nueva prueba `destination-manager-tests` cubre perfiles legacy, defaults, configuración inválida descartada, `schema_version`, versión futura, fallos de guardado y rollback de avisos de recuperación. La suite completa queda en 4 tests CTest.

## 16. Estado P1: CI y distribución Linux

Se añadió una primera capa reproducible para Linux x86_64:

- Presets `release-linux` y `ci-linux` con directorios de build separados y `CMAKE_BUILD_TYPE=Release`.
- Script `scripts/ci-linux.sh` que exige un `OBS_PREFIX` explícito, compila, ejecuta CTest, instala en staging, verifica el módulo, RPATH y dependencias, crea un tarball TGZ y genera `SHA256SUMS`.
- Workflow `.github/workflows/ci-linux.yml` para un runner Linux self-hosted con SDK OBS preparado y etiqueta `obs-sdk`.
- Metadata de instalación en `packaging/README-linux.md`.
- El tarball no incluye OBS, Qt ni bibliotecas del sistema; su compatibilidad depende del ABI del prefijo OBS usado para compilar.
- CPack genera un artefacto TGZ de staging que incluye `LICENSE` con GPL-2.0-or-later junto con la documentación. Todavía no se genera `.deb`, `.rpm`, AppImage o Flatpak.

## 17. Registro de validación de la distribución

La distribución inicial anunciada es **Linux x86_64** y está validada con **OBS Studio 32.2.2** mediante el prefijo OBS preparado usado por `scripts/ci-linux.sh`. El paquete es un tarball TGZ; no incluye OBS Studio, Qt ni bibliotecas del sistema. La compatibilidad con otras versiones de OBS no se declara hasta contar con un build y pruebas específicos.

El proyecto se distribuye bajo **GPL-2.0-or-later**. El texto completo está en `LICENSE` y los fuentes C++ llevan identificador SPDX. El titular legal de copyright y cualquier firma pública de release deben definirse antes de una publicación externa.

No se declara aún una release pública firmada: falta fijar y publicar el commit exacto de OBS/Qt usado, ejecutar smoke test en una instalación limpia de OBS y decidir firma/SBOM.