# Reutilizar la capa Android entre Matrix_Player y streamer

> Escrito desde la sesión de `streamer` el 2026-08-13, para que la sesión de
> Matrix_Player pueda ejecutarlo sin volver a investigar nada.

## Qué pasó y por qué existe este documento

`streamer` (el descargador de Qobuz, `~/Files/code/active/streamer`) se acaba de portar
a Android sobre este mismo framework. Al terminar, comparé las dos capas Android y
encontré que **cuatro problemas ya estaban resueltos dos veces**, con nombres distintos,
por la misma persona.

Este documento dice qué mover al framework, qué **no** mover, y en qué orden.

### Estado real de streamer, sin adornos

Compila y empaqueta para `arm64-v8a` y `x86_64` (APK de ~39 MB). **Nunca se ha
ejecutado**: no había dispositivo ni emulador conectado. Todo lo que sigue está validado
por el compilador y por lectura del código, no por una app corriendo.

Esto importa para el orden de trabajo: **no consolides nada en el framework hasta que
alguna de las dos apps se haya visto correr en un dispositivo.** Ya hay un precedente de
lo contrario en este repo — `framework/vk_canvas/platform/android/input_handler.hh` está
marcado *"WIP, not yet wired"* y no lo usa nadie, porque se subió una abstracción atada a
`TextBuffer`/`UndoRedo` que no le sirve a ninguna app real.

## Fase 0 — ✅ YA EJECUTADA (2026-08-13)

El merge local está hecho. Los dos checkouts del framework están en **`e926966`**
("Text input seam: whole-buffer IME edits (TextEditEvent)"), fast-forward sobre `99183f6`,
sin tocar GitHub.

Verificado después del merge, no inferido:

```
matrix_player          compila limpio (79/79)     <- la prueba que importa
streamer_gui           compila limpio
streamer_gui --selftest    ok (117 assertions)
core/input.hh          idéntico byte a byte en los dos checkouts
```

**Ese paso ya está hecho** (2026-08-13, sesión de Matrix_Player): el puntero del submódulo
y este archivo están commiteados en `a0266c1`, después de verificar contra `e926966` que
los ocho tests de escritorio pasan. El remoto local `streamer-local` quedó configurado; se
quita con `git remote remove streamer-local` cuando estorbe.

**Y una corrección al inventario de abajo, encontrada al construir:** el APK de
Matrix_Player **tampoco se había construido nunca**. `android/CMakeLists.txt` enlazaba
`ae_aaudio` y `ae_mediacodec`, y ningún CMake del repo definía ninguno de los dos. Ahora
`ae_aaudio` existe en `framework/audio_engine/CMakeLists.txt` (sólo el sink; mediacodec
queda fuera a propósito), y el APK compila y empaqueta sus assets para arm64-v8a y x86_64.
Sigue sin ejecutarse en un dispositivo, igual que el de streamer.

Lo que sigue en esta sección es el procedimiento, por si hay que repetirlo o revertirlo.

---

## Fase 0 — el procedimiento, para repetir o revertir

Los dos repos usan **el mismo submódulo**, `github.com/minervarr/Vk_Canvas_Lb_LAW`, los dos
en la rama `main`, y hoy apuntan al mismo commit:

```
Matrix_Player/framework/vk_canvas        99183f6
streamer/framework/Vk_Canvas_Lb_LAW      99183f6
```

En el checkout de streamer hay cambios sin commitear de los que depende todo lo demás —
5 archivos, 147 líneas de diff:

| Archivo | Cambio |
|---|---|
| `core/input.hh` | `struct TextEditEvent` + `InputSink::onTextEdit()` |
| `core/frame_input.hh/.cc` | `textEdited` / `editedText` / `editedCursorByte` |
| `core/widgets.hh/.cc` | `textFieldHandleInput` aplica el reemplazo del IME |

**No hay cuenta vinculada al repositorio, así que `git push` no es una opción.** No hace
falta: git acepta un directorio local como remoto, y los dos checkouts comparten historia.
Todo esto es local.

### Comprobado el 2026-08-13

Antes de escribir estos pasos verifiqué las dos mitades del mecanismo:

```
git -C Matrix_Player/framework/vk_canvas apply --check <parche>   -> aplica limpio, 5/5
git -C Matrix_Player/framework/vk_canvas fetch <ruta de streamer> main
                                                                  -> branch main -> FETCH_HEAD
```

### Camino A — con historia (preferible)

Commitear primero en el checkout de streamer. `git_wrapper commit` **no necesita remoto**;
sólo `git_wrapper push` lo necesitaría, y ese paso se omite.

Corrección a lo que quizá te dijeron antes: en esta máquina `streamer/git_wrapper` es un
**ELF de Linux**, no un `.exe`. Se ejecuta aquí perfectamente.

```bash
cd ~/Files/code/active/streamer/framework/Vk_Canvas_Lb_LAW

# git_wrapper hace "stage all", así que limpia primero lo que no quieres dentro.
# Estos .spv son artefactos de haber compilado el APK del demo del framework,
# no cambios de diseño (ver la sección de artefactos más abajo).
git checkout -- platform/android/app/src/main/assets/shaders/
rm -f platform/android/app/src/main/assets/shaders/{image,shape}_{vert,frag}.spv

git status --short          # debe listar SOLO los 5 archivos de core/

../../git_wrapper commit "Text input seam: whole-buffer IME edits (TextEditEvent)"
```

Y ahora, desde Matrix_Player:

```bash
cd ~/Files/code/active/Matrix_Player/framework/vk_canvas
git remote add streamer-local ~/Files/code/active/streamer/framework/Vk_Canvas_Lb_LAW
git fetch streamer-local main
git merge streamer-local/main        # fast-forward: mismo ancestro, sin divergencia
```

Es fast-forward mientras el checkout de Matrix siga limpio en `99183f6`. Si tuviera
commits propios, `git rebase streamer-local/main` o un `cherry-pick` del commit concreto.

El remoto local puede quedarse; es un `git remote remove streamer-local` cuando estorbe.

### Camino B — parche plano (si prefieres no commitear todavía)

Pierde la historia, pero es reversible con `git checkout --` y no toca el índice de
streamer:

```bash
git -C ~/Files/code/active/streamer/framework/Vk_Canvas_Lb_LAW diff -- core/ > /tmp/core-ime.patch
cd ~/Files/code/active/Matrix_Player/framework/vk_canvas
git apply --check /tmp/core-ime.patch     # verifica antes de tocar nada
git apply         /tmp/core-ime.patch
```

Aviso: con el Camino B los dos checkouts quedan sucios con el mismo cambio y **sin ningún
commit que los relacione**. Es fácil que uno se edite y el otro se quede atrás sin que
nada lo señale. Úsalo sólo como paso temporal.

### Cuando aparezca la cuenta

Ambos caminos dejan el trabajo íntegro y listo para subir. `git_wrapper push` desde
cualquiera de los dos checkouts sincroniza `origin` y el otro se pone al día con un
`git pull` normal.

### Por qué esto no rompe el escritorio

Los backends de Wayland y Win32 no emiten el evento nuevo — `onTextEdit` tiene
implementación vacía por defecto en `InputSink`, así que nada que ya compilara deja de
hacerlo. `streamer_gui --selftest` sigue en 117 asserts (eran 108 antes de las 9
aserciones nuevas sobre composición de IME).

## Los `.spv`: artefactos, no cambios

Además de los 5 archivos de `core/`, el checkout del framework en streamer tiene 10 `.spv`
tocados:

```
platform/android/app/src/main/assets/shaders/    6 modificados + 4 sin seguimiento
```

Salieron de compilar el APK del demo del propio framework para validar el toolchain: su
CMake escribe los shaders compilados **dentro de su árbol de fuentes**, que es donde AGP
los busca para empaquetarlos. No son cambios de diseño y **no deben viajar a Matrix**.

Los 4 sin seguimiento (`image_*`, `shape_*`) son shaders que el demo compila pero que nunca
se habían commiteado. Volverán a aparecer cada vez que alguien compile el demo. Si molesta
de forma recurrente, la solución real es un `.gitignore` en esa carpeta y no commitear
binarios generados — pero eso es una decisión aparte de este trabajo, y afecta a cómo el
demo se distribuye.

Nada de esto afecta a streamer ni a Matrix: cada app compila sus shaders en su propio
árbol (`streamer/android/app/src/main/assets/shaders`), no en el del framework.

## El inventario: qué está duplicado

| Problema | Matrix_Player | streamer |
|---|---|---|
| Rutas de escritura/config | `android/src/app_paths_android.{hh,cc}` | `config::set_platform_dirs()` en `src/config.cpp` |
| Permiso de todos los archivos | `android/src/storage_permission.{hh,cc}` | `requestStoragePermission()` en Java |
| Insets (notch, barras, IME) | `android/src/safe_area.{hh,cc}` | `nativeOnInsets` → `AppHost::safe_area()` |
| Abrir URL / leer Intent | `android/src/launch_intent.{hh,cc}` | `openUrl()` en Java |

Y estas piezas **solo existen en streamer**, en `gui/src/os/android_host.cc`:

| Pieza | Líneas | Nota |
|---|---|---|
| `utf16_to_utf8` / `to_jstring` | 86-118, 170-195 | Evita el *modified UTF-8* de JNI, que parte los emoji y todo lo fuera del BMP en dos caracteres rotos |
| Helpers `call_void` / `call_string` / `call_with_string` | 127-206 | Sobre `jni_util.hh`, con `check_exc` en cada llamada |
| **Gesto táctil → puntero + rueda** | 208-232, 410-466 | Slop de 24 px; por debajo es un toque, por encima es scroll y el toque se cancela |
| Ciclo de vida de la superficie | 356-375 | `create_surface`/`destroy_surface` en `APP_CMD_INIT/TERM_WINDOW` |
| Mapa de teclas Android → `key::*` | 468-505 | |
| Puente del IME (C++ + Java) | 56-84, 377-408, 531-583 | + `android/app/src/main/java/io/nava/streamer/StreamerActivity.java` |

Contraprueba de que el framework es el destino correcto: `fullscreen.hh` y `orientation.hh`
**ya están** en `platform/android/`. Estos cuatro simplemente nunca migraron.

## La decisión de diseño: NO subas `Host`

Comparé las dos interfaces:

- **streamer** `gui/src/host.hh`: 11 métodos. Ventana, `pump`, portapapeles, `beep`.
- **Matrix_Player** `gui/src/host.hh`: 20. Monitores, `snapToEdge`, `setKeepAwake`,
  `setCursor`, timers, `HWND`, `showErrorMessage`.

**No convergieron, y no van a converger.** El seam describe lo que *esa aplicación*
necesita del sistema operativo, y un reproductor y un descargador necesitan cosas
distintas. Subirlo daría la unión de ambas, que le queda mal a las dos, y cada app nueva
la ensancharía más.

**Sube mecanismos, no arquitectura.** Funciones libres y clases pequeñas sin opinión sobre
cómo es tu app. Cada app conserva su `Host` propio y delgado, que las compone.

### La tensión que hay que resolver primero: ¿Java sí o no?

Es la diferencia estructural entre las dos apps y hay que decidirla antes de escribir
código:

- **Matrix_Player usa `android.app.NativeActivity` directamente. Cero Java.** Sus helpers
  son nativos puros: `query_safe_area_insets(android_app*)`, `request_all_files_access(android_app*)`.
- **streamer subclasea con `StreamerActivity extends NativeActivity`**, porque el IME lo
  obliga: un `EditText` real fuera de pantalla es dueño del buffer, y una vista que no
  está en el layout no es un destino válido para el teclado.

**Recomendación:** el camino nativo puro de Matrix es el mejor y debe ser el
predeterminado. La actividad Java es *opt-in* y solo hace falta para entrada de texto. El
framework debería ofrecer las dos, no forzar la segunda.

Consecuencia concreta en la forma de la API: los insets de Matrix son **pull**
(`query_safe_area_insets()`), los míos son **push** (`nativeOnInsets` desde un listener).
Pull es más simple y no necesita Java. Push es más correcto cuando hay IME, porque los
insets cambian al abrirse y cerrarse el teclado. **Ofrece `query_safe_area_insets()` como
API principal y deja el push como refinamiento opcional de la ruta Java.**

## Plan de extracción, en orden

Todo va a `framework/vk_canvas/platform/android/`.

**Fase 0 — desbloquear.** Traer los 5 archivos de `core/` desde el checkout de streamer
(ver *Fase 0* arriba: merge local, sin GitHub). Sin esto no arranca nada.

**Fase 1 — lo que ya está duplicado y no necesita Java.** El riesgo más bajo y el valor
más alto, porque hay dos implementaciones que comparar y quedarse con la mejor.

```
platform/android/app_paths.{hh,cc}          <- app_paths_android.* de Matrix
platform/android/storage_permission.{hh,cc} <- tal cual de Matrix
platform/android/safe_area.{hh,cc}          <- tal cual de Matrix (forma pull)
platform/android/launch_intent.{hh,cc}      <- tal cual de Matrix, + open_url()
```

Matrix se queda casi igual: cambian los `#include` y se borran cuatro pares de archivos.
En streamer, `android_host.cc` empieza a llamarlos en vez de hacer JNI a mano.

**Fase 2 — el sustrato JNI.** Ampliar `jni_util.hh`, que ya existe y ya tiene `env_for` y
`check_exc`:

```cpp
namespace vce::platform::jni {
  std::string utf16_to_utf8(const jchar*, jsize, jsize cursorUnits, size_t* cursorByte);
  jstring     to_jstring(JNIEnv*, const std::string& utf8);
  bool        call_void  (android_app*, const char* method, const char* sig, jvalue*);
  std::string call_string(android_app*, const char* method);
}
```

Ojo con lo obvio-pero-no: `GetStringUTFChars` y `NewStringUTF` hablan *modified UTF-8*, no
UTF-8. Un título con emoji sale roto. Por eso estas funciones existen en vez de usar las
de JNI directamente.

**Fase 3 — el gesto táctil.** Es la pieza más valiosa y la que más se va a reutilizar,
porque no depende de nada:

```cpp
// platform/android/touch_input.hh
class TouchTranslator {
 public:
  // Sintetiza el flujo de puntero/rueda que espera InputSink a partir de un
  // AInputEvent. Un dedo no es un ratón: arrastrar debe hacer scroll, no
  // pulsar lo que hubiera debajo al empezar.
  bool handle(AInputEvent*, InputSink&);
  float tapSlopPx = 24.0f;
};
```

Regla implementada: nada se emite al tocar; si el dedo supera el slop pasa a modo scroll y
emite `WheelEvent`; si se levanta sin superarlo, emite `Down` y `Up` en el mismo frame.
Mapa de teclas al lado, en el mismo archivo.

**Fase 4 — el IME. Solo cuando haga falta.** Matrix_Player **no tiene ni un campo de
texto** hoy (`TextFieldState` no aparece en su `gui/src/`), así que esto no le aporta nada
todavía. Súbelo el día que gane un buscador. Cuando llegue: el Java es reutilizable casi
entero, solo cambian el paquete y el nombre de la clase; y la mitad C++ es
`PendingText` + `drain_pending_text` + las cuatro funciones JNI.

## Trampas encontradas en el port, para no repetirlas

Ninguna de estas se anuncia con un error claro:

- **AGP fusiona los assets *antes* de compilar lo nativo.** Fuentes y shaders generados no
  entran en el primer APK y aparecen en el siguiente — con la build reportando éxito y los
  archivos visibles en disco. Un APK sin fuentes arranca en negro. Arreglo en
  `android/app/build.gradle`: hacer que `merge*Assets` dependa de `buildCMake*`.
- **`CMAKE_SOURCE_DIR` apunta a `android/`**, no a la raíz del repo, si el `CMakeLists.txt`
  de arriba es el de Gradle. Todo path de assets construido con él resuelve a un archivo
  inexistente, en silencio. Usa una variable anclada al directorio del propio `CMakeLists`.
- **`-u ANativeActivity_onCreate` es obligatorio.** Nada en tu código lo referencia, el
  linker lo recolecta, y la app muere al arrancar en negro sin pista alguna en tiempo de
  compilación.
- **`optional::value_or({})` no compila con libc++** (la libstdc++ de GCC lo acepta como
  extensión). No hay tipo del que deducir `U&&`. Igual: `decltype(x)::value_type` necesita
  `typename`. El NDK destapa código no portable que Linux escondía.
- **`VceShaders.cmake` cae a una ruta fija de Windows** si `$VULKAN_SDK` no existe. Pasa
  `-DVCE_SLANGC=` explícito desde Gradle.
- **LTO se desactiva sola bajo el NDK** (`ld.gold` no encuentra `LLVMgold.so`). Sale un
  error rojo por consola y la build continúa correctamente; no lo persigas.

## Cómo verificar que la extracción no rompió nada

En cada fase, las dos apps:

```bash
# streamer, escritorio — la línea base a no mover
scripts/linux/build.sh --debug -DSTREAMER_GUI=ON -DVCE_SLANGC=/opt/shader-slang/bin/slangc
build/linux_debug/gui/streamer_gui --selftest      # debe decir: ok (117 assertions)

# streamer, Android
cd android && VULKAN_SDK=/opt/shader-slang sh gradlew assembleDebug --no-daemon
# y comprobar que el APK lleva los assets, no solo que la build diga SUCCESSFUL:
unzip -l app/build/outputs/apk/debug/app-arm64-v8a-debug.apk | grep -cE "assets/fonts/"   # 9
```

`gradlew` no tiene bit de ejecución en el repo; se invoca con `sh gradlew`.

Y lo más importante: **instalar en un dispositivo real y mirar `logcat`.** Nada de esto
está probado en ejecución todavía.

## Lo que queda fuera

macOS e iOS. No hay backend Metal/MoltenVK, y ahí el seam sí se ensancha de verdad — no
es el mismo tipo de trabajo que Android, donde Vulkan y el motor de fuentes cruzaron sin
tocarse.
