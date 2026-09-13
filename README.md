# OBS Multistream RTMP

Plugin nativo para OBS Studio 32.2.2 que transmite localmente a varios destinos RTMP desde un dock Qt. Cada destino usa el output RTMP oficial de OBS, mientras que los outputs comparten los encoders de video y audio del streaming configurado en OBS.

## Estado

MVP inicial en desarrollo:

- C/C++17 y Qt 6 (Widgets, Network y Svg); OAuth no usa WebEngine embebido.
- Windows, macOS y Linux mediante CMake; el almacén seguro está implementado para Linux mediante Secret Service/libsecret.
- Destinos RTMP/RTMPS configurables manualmente.
- Destino Facebook Pages y timeline personal mediante OAuth de escritorio, selección de destino y creación automática de Live Video.
- Destino Twitch mediante OAuth implícito en el navegador del sistema, actualización opcional del título y obtención segura de la stream key.
- Destino YouTube Live mediante OAuth PKCE en el navegador del sistema, creación automática de broadcast + stream + bind y finalización al detener.
- Título, descripción y privacidad de Facebook y YouTube enviados a sus APIs respectivas.
- Los Client ID son públicos; no se solicita ni se incluye ningún App Secret o client secret.
- Persistencia por perfil de OBS, inicio/detención global y estado independiente por destino.

Los tres logins se abren en el navegador predeterminado del sistema. El plugin levanta un servidor HTTP temporal en `127.0.0.1`, valida `state` y captura el callback sin WebEngine. Google usa authorization code + PKCE; Facebook y Twitch usan implicit grant con una página local que reenvía el fragmento OAuth. No se abre un productor externo ni se introducen manualmente la URL RTMP o la clave.

## Requisitos

- OBS Studio 32.2.2 (Linux x86_64 release validation; other versions are not guaranteed).
- CMake 3.28 o superior.
- Qt 6 (Core, Widgets, Network y Svg), `libsecret` y los paquetes de desarrollo de OBS (`libobs` y `obs-frontend-api`). En Debian/Ubuntu, instala los paquetes de desarrollo de Qt 6 y `libsecret-1-dev`. OAuth se abre en el navegador predeterminado del sistema; no se necesita Qt WebEngine.

## Licencia

El proyecto se distribuye bajo **GPL-2.0-or-later**. El texto completo está en `LICENSE` y los fuentes C++ incluyen identificadores SPDX.

## Compilación

La compilación necesita un prefijo de OBS que contenga `libobs`, `obs-frontend-api` y sus archivos CMake. No se debe asumir una instalación de OBS del sistema; pásala explícitamente mediante `CMAKE_PREFIX_PATH`.

Para desarrollo Debug:

```bash
cmake --preset default -DCMAKE_PREFIX_PATH=/ruta/a/obs/install \
    -DTWITCH_CLIENT_ID=tu_client_id \
    -DYOUTUBE_CLIENT_ID=tu_google_desktop_client_id
cmake --build --preset default
ctest --preset default
```

Para una build Release local:

```bash
cmake --preset release-linux -DCMAKE_PREFIX_PATH=/ruta/a/obs/install
cmake --build --preset release-linux
ctest --preset release-linux
```

Para validar el staging y crear artefactos Linux reproducibles desde un SDK OBS preparado:

```bash
OBS_PREFIX=/ruta/a/obs/install bash scripts/ci-linux.sh
```

El script ejecuta build Release, CTest, instalación en `dist/stage`, comprobación del módulo y sus dependencias, creación del tarball TGZ y generación de `SHA256SUMS`. Para crear el paquete Debian público, usa `scripts/package-deb.sh` con `OBS_PREFIX`, `YOUTUBE_CLIENT_ID` y `YOUTUBE_CLIENT_SECRET` definidos sólo en el entorno local o protegido del pipeline. El `.deb` instala el plugin en una instalación normal de OBS 32.2.2.

El workflow de GitHub Actions requiere un runner Linux self-hosted con la etiqueta `obs-sdk` y la variable de repositorio `OBS_PREFIX`; un runner limpio no trae `libobs` ni `obs-frontend-api`.

Para instalar manualmente un build:

```bash
cmake --install build/release-linux --prefix /ruta/al/prefijo
```

El plugin no implementa ni copia el protocolo RTMP: reutiliza el `rtmp_output` oficial de OBS para cada destino.

## Facebook OAuth

El login se ejecuta en el navegador predeterminado del sistema mediante un servidor HTTP temporal en `127.0.0.1`; no se usa Qt WebEngine. La aplicación Meta debe permitir la URI de loopback que utilice el plugin, por ejemplo `http://127.0.0.1` o `http://localhost` según la configuración de la app. El puerto local se asigna temporalmente y el plugin valida `state` antes de aceptar el token.

El plugin usa el flujo OAuth implícito para no requerir App Secret. El token aparece en el fragmento OAuth, se captura mediante una página local de puente JavaScript, se guarda en memoria y se persiste mediante Secret Service/libsecret en Linux.

1. Abre el dock `Multistream RTMP` desde el menú **Docks** de OBS.
2. Agrega un destino y selecciona **Facebook** en **Proveedor**.
3. Pulsa **Conectar Facebook**. El navegador predeterminado abrirá el login; acepta los permisos solicitados.
4. El plugin detectará automáticamente la autorización y cargará las Páginas disponibles.
5. Selecciona **Página** o **Mi timeline**, elige la Página si corresponde y escribe el título y, opcionalmente, la descripción y la privacidad.
6. Pulsa **Iniciar**. El plugin crea el Live Video con Graph API, obtiene automáticamente `secure_stream_url` y arranca el RTMP de OBS.
7. Pulsa **Detener** para parar el output y finalizar el Live Video conservando el VOD.

Los tokens y las claves dinámicas de Facebook no se guardan en el JSON del perfil. Desconectar Facebook elimina el token de la memoria y del almacén seguro.

Para destinos genéricos, introduce la URL RTMP y el stream key como antes.

## Preparación de la app Meta para producción

La app Meta usada por el plugin es `1609511030686437`. Antes de activar **Live Mode**, completar esta lista en Meta for Developers:

1. En **Casos de uso**, mantener **Accede a la API de video en vivo** y **Administrar todos los aspectos de tu página**.
2. Confirmar acceso avanzado/revisión para los permisos mínimos que realmente usa el plugin:
   - `pages_show_list`: mostrar las Pages administradas y permitir seleccionar una.
   - `pages_read_engagement`: permiso requerido por Meta para el flujo de Live Video de una Page y para leer sus metadatos necesarios.
   - `pages_manage_posts`: permiso requerido por Meta para crear el objeto Live Video en `/{PAGE_ID}/live_videos`; el plugin no lo usa para crear publicaciones normales.
   - `publish_video`: crear Live Videos en el timeline personal mediante `/me/live_videos`.
3. Configurar como política de privacidad pública la URL:
   `https://pentaa555.github.io/penta-multistream-legal/`
4. Configurar como **URL de instrucciones de eliminación de datos** la URL:
   `https://pentaa555.github.io/penta-multistream-legal/data-deletion.html`
   La implementación actual ofrece instrucciones públicas y atención por correo; no expone todavía un endpoint callback firmado. Meta permite usar una URL de instrucciones o una callback, por lo que esta URL no debe registrarse como callback. Después de publicar cambios en el repositorio legal, comprobar que ambas URLs ya muestran el texto actualizado.
5. En **Facebook Login / OAuth**, registrar una URI de loopback HTTP permitida por la aplicación, por ejemplo:
   `http://127.0.0.1`
   El plugin usa un puerto local temporal y valida el callback mediante `state`.
6. En **Configuración > Básica**, completar correo de contacto, categoría e icono de aplicación de 1024x1024.
7. Para la revisión, grabar en inglés o con subtítulos el flujo completo: autorización, selección de Página, selección de timeline, título/privacidad, creación del Live Video, recepción de señal RTMPS y detención.
8. Ejecutar y conservar al menos una llamada Graph exitosa por cada permiso solicitado dentro de los 30 días posteriores a enviar la solicitud de revisión. No solicitar permisos que no aparezcan en la demostración.
9. Enviar la revisión de la app y activar Live Mode sólo después de que Meta apruebe los permisos. Las cuentas sin rol en la app o en el negocio conectado no podrán usarlos mientras la app siga en desarrollo.

Referencias oficiales:
- https://developers.facebook.com/docs/development/create-an-app/
- https://developers.facebook.com/docs/app-review/submission-guide/
- https://developers.facebook.com/docs/development/create-an-app/app-dashboard/data-deletion-callback
- https://developers.facebook.com/docs/pages/overview/permissions-features/
- https://developers.facebook.com/docs/live-video-api/guides/streaming/

El plugin no incluye el App Secret. En la versión Linux validada, los tokens de Facebook se mantienen en memoria mientras OBS está activo y se persisten únicamente en el almacén seguro del sistema mediante Secret Service/libsecret; nunca se escriben en el JSON del perfil, logs ni este repositorio.

## Twitch OAuth

Twitch requiere registrar una aplicación en el portal de desarrolladores y pasar su **Client ID público** al configurar CMake. No se usa ni se solicita un Client Secret:

```bash
cmake --preset default -DCMAKE_PREFIX_PATH=/ruta/a/obs/install -DTWITCH_CLIENT_ID=tu_client_id
```

La aplicación debe registrar una URI de redirección HTTP de loopback, por ejemplo:

`http://127.0.0.1`

El puerto local se asigna temporalmente y el plugin valida `state`.

El flujo solicita `channel:read:stream_key` y `channel:manage:broadcast`. El token se captura en el fragmento OAuth, se valida mediante `state` y, en Linux, se persiste sólo en Secret Service/libsecret. Al iniciar una transmisión el plugin actualiza el título configurado, consulta `/helix/streams/key` y usa `rtmps://live.twitch.tv/app/` con la clave únicamente en memoria. La clave no se guarda en el perfil de OBS, logs ni archivos del plugin.

1. Registra la aplicación Twitch con la URI anterior.
2. Configura `TWITCH_CLIENT_ID` en la build y reinstala el plugin si cambia.
3. Agrega un destino Twitch, pulsa **Conectar Twitch** y autoriza los permisos.
4. Escribe el título y pulsa **Iniciar**; OBS obtendrá la clave y abrirá el output RTMPS automáticamente.
5. Al detener OBS se detiene el output Twitch; no se crea un recurso Live remoto que requiera una llamada de finalización adicional.

La persistencia segura de tokens está implementada para Linux/libsecret, igual que la integración Facebook. Windows y macOS siguen requiriendo una implementación de almacén seguro antes de declararse plataformas soportadas.

## YouTube Live

YouTube requiere un proyecto de Google Cloud, pero para esta integración no se necesita contratar servidores ni activar servicios facturables. La cuota de YouTube Data API v3 es gratuita y limita el número de operaciones diarias.

1. En Google Cloud Console crea un proyecto o usa uno existente.
2. Habilita **YouTube Data API v3**.
3. Configura la pantalla de consentimiento OAuth. Para pruebas, agrega tu cuenta como usuario de prueba.
4. Crea credenciales OAuth 2.0 de tipo **Desktop app** y copia el Client ID.
5. Compila el plugin con:

   ```bash
   cmake --preset default -DCMAKE_PREFIX_PATH=/ruta/a/obs/install \
       -DTWITCH_CLIENT_ID=tu_client_id \
       -DYOUTUBE_CLIENT_ID=tu_google_desktop_client_id
   ```

El login de YouTube se abre en el navegador predeterminado mediante OAuth authorization code + PKCE. El plugin levanta un callback temporal en `http://127.0.0.1:{puerto}` y no usa Qt WebEngine. Normalmente el intercambio PKCE no necesita `client_secret`; si una credencial Desktop concreta devuelve `client_secret is missing`, el plugin puede cargarlo una sola vez desde Secret Service/libsecret mediante la variable de entorno `YOUTUBE_CLIENT_SECRET`.

Para configurar una instalación local sin escribir el secreto en el historial del shell:

```bash
read -s YOUTUBE_CLIENT_SECRET
env YOUTUBE_CLIENT_SECRET="$YOUTUBE_CLIENT_SECRET" /ruta/a/obs/bin/obs
set -e YOUTUBE_CLIENT_SECRET
```

El plugin guarda ese valor en libsecret para las siguientes ejecuciones. No lo compartas, no lo incluyas en el repositorio y no lo guardes en el perfil de OBS. En una distribución pública no se debe pedir este valor a cada usuario: el Client ID y cualquier credencial Desktop estática son credenciales de un public client; la protección real es PKCE + `state` + loopback. Para un artefacto público, el pipeline de release puede inyectar opcionalmente la credencial Desktop durante la configuración, sin escribirla en Git:

```bash
read -s YOUTUBE_CLIENT_SECRET
env YOUTUBE_CLIENT_SECRET="$YOUTUBE_CLIENT_SECRET" cmake --preset release-linux \
    -DCMAKE_PREFIX_PATH=/ruta/a/obs/install \
    -DYOUTUBE_CLIENT_ID=tu_google_desktop_client_id
set -e YOUTUBE_CLIENT_SECRET
```

Ese valor queda dentro del binario de la aplicación Desktop si Google lo exige; no debe tratarse como un secreto de servidor porque un public client no puede ocultarlo. Nunca se debe distribuir como una contraseña de usuario ni incluirlo en el repositorio. Lo que sí es secreto por usuario son los refresh tokens, que se guardan en el almacén seguro del sistema.

### Preparación para publicación pública de YouTube

En Google Auth Platform completa, en este orden:

1. **Público**: configura la aplicación como `External`, nombre, correo de soporte, correo del desarrollador, página de inicio y política de privacidad.
2. **Acceso a los datos**: añade exactamente `https://www.googleapis.com/auth/youtube` y no solicites scopes que el plugin no use.
3. **Centro de verificación**: inicia la verificación OAuth para el scope sensible de YouTube. Prepara una descripción del flujo, la política de privacidad, instrucciones de eliminación de datos y un vídeo de demostración del login, creación del directo y detención.
4. Mientras la verificación esté pendiente, añade las cuentas de prueba en **Público → Usuarios de prueba**. Los usuarios generales no deben depender de este modo de prueba.
5. Usa una credencial Desktop de producción y conserva una credencial/proyecto separado para desarrollo cuando sea posible.

Hasta que Google apruebe la verificación, la distribución debe considerarse beta y limitarse a usuarios de prueba.

1. Agrega un destino y selecciona **YouTube** en **Proveedor**.
2. Pulsa **Conectar YouTube** y autoriza el acceso al canal.
3. Configura título, descripción y privacidad (`Public`, `Unlisted` o `Private`).
4. Al iniciar, el plugin crea el broadcast, crea el stream RTMP, enlaza ambos recursos y configura automáticamente el servidor y la clave en OBS.
5. Al detener, el plugin transiciona el broadcast a `complete` para finalizar el directo.

El refresh token de YouTube se guarda únicamente en el almacén seguro del sistema. El access token, la URL de ingestión, la stream key y los IDs de broadcast/stream se mantienen en memoria; no se escriben en el perfil de OBS ni en logs.