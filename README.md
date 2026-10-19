# ARIA — Advanced Reforger Intelligent AI

Mod de IA para **Arma Reforger** (1.8.x). Hace que la IA pelee en equipo, use
vehículos como corresponde y, como función nueva, **pilote helicópteros**: cosa que la IA
vanilla de Reforger no sabe hacer. Todo se puede configurar sin recompilar.

ARIA no reemplaza la IA vanilla: la **dirige**. Cada soldado conserva su puntería, sus
coberturas y su lógica de equipos de fuego nativa. ARIA añade lo que le falta: alguien que
piense a nivel de escuadra, que comparta información entre escuadras, que use los
vehículos con cabeza y que vuele.

---

## ⚠️ Estado del proyecto (léelo primero)

| | |
|---|---|
| API usada | Verificada contra los scripts oficiales de Bohemia **1.8.0.13** ([Arma-Reforger-Script-Diff](https://github.com/BohemiaInteractive/Arma-Reforger-Script-Diff)) |
| Verificador estático (`tools/aria_lint.py`) | **0 errores**. Revisa clases, métodos, campos, enums, número de argumentos, overrides y acceso `protected` contra ~9000 clases vanilla |
| Simulador de vuelo (`tools/flight_sim.py`) | **PASA**: despegue, crucero de 3 km, vuelo estacionario, órbita y aterrizaje, con viento y framerate variable |
| Revisiones independientes | 3 rondas (63 agentes en la última, cada hallazgo verificado por 2 escépticos): unos 40 errores de ejecución encontrados y **corregidos**, entre ellos contactos fantasma entre escuadras, retiradas en cadena, helicópteros sin motor que seguían volando y aerotransportes que no terminaban |
| **Compilado en Workbench** | **Todavía no.** Este código se escribió sin acceso al juego ni a Workbench |
| **Probado en partida** | **Todavía no** |

Lo más probable es que la primera compilación en Workbench muestre algunos errores
pequeños: sintaxis que el verificador no cubre, o conversiones de tipos. Si te pasa, copia el
error de la consola de Workbench (archivo:línea) y se corrige en minutos. Toda la lógica
que depende del motor está aislada en pocos archivos (ver *Arquitectura*), así que los
arreglos quedan localizados.

---

## Qué hace

### Escuadras de infantería (al estilo LAMBS / VCOM, integradas con la IA nativa)
- **Memoria de contactos**: cada escuadra recuerda dónde vio al enemigo, cuándo y de qué tipo es (infantería, blindado, aéreo, estático), y lo va olvidando con el tiempo.
- **Inteligencia por radio**: la escuadra que detecta al enemigo lo reporta a las escuadras amigas en rango, con un retardo de radio. La información entra directo en la **percepción nativa** del grupo receptor, así que la IA vanilla investiga, suprime o ataca por sí misma.
- **Decisiones de combate**, según la relación de fuerzas y el perfil de habilidad de cada facción:
  - **Flanqueo**: busca el flanco más oculto analizando líneas de visión del terreno, avanza por una ruta cubierta y luego barre la posición enemiga.
  - **Mantener y suprimir**: si está superada, no avanza a cuentagotas; se queda en su sitio y suprime.
  - **Retirada**: si está rota (bajas o enemigo abrumador), se repliega a un punto fuera de la vista del enemigo y pide ayuda.
- **Refuerzos**: las escuadras ociosas cercanas acuden a ayudar y se colocan en la línea, no delante del enemigo.
- **Apoyo aéreo cercano** automático cuando aparecen blindados, aeronaves o fuerzas grandes.
- **Armas estáticas**: en combate, la escuadra pone un artillero en ametralladoras o lanzadores fijos libres que tenga cerca.
- **Respeta las órdenes**: los waypoints forzados del Game Master o de la misión (prioridad > 0) nunca se sobrescriben.
- **No toca la IA de los jugadores**: con `squad.affectPlayerLedGroups = false`, los grupos con jugadores y la IA reclutada por ellos se devuelven a la IA vanilla.

### Vehículos terrestres (coches, camiones, APC, IFV, tanques)
- **Uso de vehículos**: para movimientos largos, la escuadra toma un vehículo amigo o neutral libre que tenga cerca y la IA vanilla lo conduce.
- **Procedimiento de desembarco**: los transportes descargan a la infantería antes de entrar en la zona de fuego. En vehículos armados, conductor y artillero se quedan para dar apoyo.
- **Torretas**: en contacto, los pasajeros ocupan las torretas vacías.
- **Abandono**: la tripulación sale de vehículos inutilizados (motor destruido o casco muy dañado).
- **Convoyes**: limita la velocidad cuando la escuadra usa varios vehículos.
- **Recuperación**: los vehículos atascados retroceden y giran para liberarse, y los volcados se enderezan. La IA vanilla, en cambio, los abandona para siempre.

### 🚁 Helicópteros: función NUEVA
La IA vanilla de Reforger 1.8 **no puede volar**. Con ARIA, cualquier helicóptero (vanilla o
de mods) cuyo asiento de piloto ocupe un soldado de IA de un grupo dirigido por IA vuela solo:

- **Piloto automático en cascada** (posición → velocidad → aceleración → actitud): inclina el rotor para acelerar, como un helicóptero real.
- **Seguimiento del terreno**, contando árboles y edificios, más una sonda anticolisión frontal.
- **Ciclo completo de vuelo**: arranque del motor, espera hasta que el rotor alcanza sus RPM, despegue vertical, crucero, aproximación en planeo, **búsqueda automática de zona de aterrizaje** (pendiente, agua, árboles, edificios) y aterrizaje suave.
- **Waypoints vanilla** (ver tabla abajo): Move, Patrol, Cycle, Defend, Search & Destroy, Get Out, Get In, Wait...
- **Misiones de apoyo automáticas**:
  - **Aerotransporte**: una escuadra con un objetivo lejano llama a un helicóptero libre, que aterriza a su lado, la embarca, la deja cerca del objetivo y vuelve a la base.
  - **CAS**: un helicóptero armado orbita sobre el enemigo con los artilleros de puerta del lado del objetivo.
- **Supervivencia**:
  - Si recibe impactos, maniobra evasiva a baja cota.
  - Si queda muy dañado, desembarca a los pasajeros y vuelve a la base (RTB).
  - Si el piloto muere, el **copiloto toma los mandos**.
  - Si no encuentra dónde aterrizar, marca el punto como malo, busca en un radio mayor y, tras 3 intentos, desiste de ese aterrizaje.
  - Sin combustible, con el motor ahogado o destruido, o con un rotor destruido, ARIA no lo sostiene en el aire: lo suelta y cae según la física real del juego.
- **Nunca toca helicópteros pilotados por jugadores.**

### Totalmente personalizable
- **Presets**: `CASUAL`, `REALISTIC` (por defecto), `ELITE` y `CUSTOM`.
- **Perfiles por facción**:
  - `skill`: precisión.
  - `perception`: capacidad de detección.
  - `reactionTime`: tiempo de reacción.
  - `courage`: aguante antes de retirarse.
  - `aggression`: tendencia a flanquear.
  - `teamwork`: tendencia a compartir información y acudir en ayuda.
- **Más de 80 ajustes** en un archivo de texto documentado, editables en caliente con comandos de admin.

---

## Instalación

### Desde el código (Workbench)
1. Clona este repositorio en una carpeta, por ejemplo `Documents/My Games/ArmaReforgerWorkbench/addons/ARIA`.
2. Abre **Arma Reforger Tools** (Workbench), pulsa **Add Existing Project** y selecciona `addon.gproj`.
3. Abre el proyecto y, en el **Script Editor**, compila con **Build → Compile and Reload Scripts**.
   - Si hay errores, la consola muestra `archivo.c:línea`.
4. Pruébalo: abre un mundo (Everon o Arland), coloca grupos de IA y pulsa **Play**.
   - En la consola deberías ver `[ARIA] ARIA started ...`.
5. Para publicar: **Workbench → Publish Project**. En el servidor, añade el mod a la lista `mods` del `config.json` (con el GUID que asigne el Workshop) y declara como dependencia la versión base del juego.

> El `addon.gproj` incluido tiene un GUID aleatorio válido. Si prefieres que lo genere
> Workbench, crea un proyecto nuevo y copia dentro la carpeta `Scripts/`.

### Sin prefabs que modificar
ARIA se activa solo con `modded class` sobre el modo de juego, así que funciona en Conflict,
Game Master, Combat Ops y escenarios propios sin tocar ningún prefab. Solo se ejecuta en el
**servidor** (o en el host / singleplayer).

---

## Guía para creadores de misiones

### Escuadras y vehículos
No hay que hacer nada: coloca grupos y waypoints como siempre. Si dejas un vehículo libre
cerca de una escuadra que tiene un waypoint lejano, la escuadra lo usará.

### Helicópteros
1. Coloca el helicóptero y mete en el **asiento del piloto** a un soldado de IA de un grupo dirigido por IA. En Game Master basta con "ocupar vehículo" con la tripulación por defecto. Los artilleros de puerta van en las torretas.
2. Da waypoints **al grupo de la tripulación**. ARIA los traduce así:

| Waypoint vanilla | Qué hace el helicóptero |
|---|---|
| Move / Forced Move / Patrol | Vuela hasta allí. En el último waypoint, si lleva tropas, aterriza y las desembarca; si no, se queda en vuelo estacionario |
| Cycle | Recorre la ruta de patrulla en bucle infinito |
| Defend | Orbita la zona (vigilancia) |
| Search & Destroy / Attack / Suppress | Órbita CAS durante `casStationTime` segundos |
| Get Out | Busca una LZ segura, aterriza y desembarca a todos los pasajeros |
| Get In | Aterriza y espera a que suban pasajeros |
| Wait / Loiter / Observation | Vuelo estacionario durante el tiempo de espera del waypoint |

3. **Helicóptero de apoyo bajo demanda**: deja un helicóptero con tripulación de IA **sin waypoints** en una base. Así queda disponible para:
   - **aerotransportar** escuadras de su facción con objetivos a más de `airliftMinDistance` (2 km por defecto);
   - dar **CAS** a escuadras en apuros, si está armado.

> Nota: el helicóptero toma los waypoints del grupo de la tripulación y los vacía, para que la
> IA vanilla no intente "caminar" con él. En Game Master, esos waypoints dejan de verse en
> el mapa una vez asignados.

### API de scripting
```c
ARIA_Api.HeliMove(heli, "4200 0 6100");          // volar y quedarse en estacionario
ARIA_Api.HeliLandAndUnload(heli, lzPos);           // inserción
ARIA_Api.HeliOrbit(heli, targetPos, 500, 300);     // 5 minutos de órbita de ataque
ARIA_Api.HeliReturnToBase(heli);
ARIA_Api.RequestAirlift(group, objectivePos);      // "taxi" para una escuadra
ARIA_Api.RequestCAS(group, enemyPos);
ARIA_Api.ShareIntel(faction, enemyEntity, pos);    // revelar un enemigo por radio
ARIA_Api.GetSettings().m_Heli.m_fCruiseSpeedKmh = 200;
ARIA_Api.NotifySettingsChanged();
```

---

## Configuración

Al primer arranque, ARIA escribe un archivo documentado en:

- Servidor dedicado: `<carpeta de perfil>/ARIA/ARIA_Settings.cfg`
- Workbench / local: `<perfil de usuario>/ARIA/ARIA_Settings.cfg`

Tienes una copia completa en [`docs/ARIA_Settings.example.cfg`](docs/ARIA_Settings.example.cfg).

- El **preset** fija la base y todo lo demás viene comentado con `#`.
- Para cambiar un valor, quita el `#` de su línea.
- Los valores fuera de rango se recortan, y las claves mal escritas se avisan en el log.

Ejemplos:
```ini
general.preset = ELITE
general.excludedFactions = CIV

# Los soviéticos más aguerridos que los FIA
skill.USSR.skill = 0.85
skill.USSR.courage = 0.9
skill.FIA.skill = 0.35
skill.FIA.aggression = 0.7

heli.cruiseSpeedKmh = 200
heli.autoAirlift = true
squad.intelRadioDelay = 8
vehicle.dismountDistance = 500
```

### Comandos de administrador (chat o RCON)
| Comando | Efecto |
|---|---|
| `#aria status` | Escuadras gestionadas y en combate, y helicópteros en vuelo |
| `#aria reload` | Relee el archivo de configuración. Si ARIA estaba apagado (`general.enabled = false`) y lo activaste en el archivo, lo arranca. Si el archivo no se puede leer, mantiene la configuración actual |
| `#aria set <clave> <valor>` | Cambia un ajuste en caliente, por ejemplo `#aria set heli.cruiseSpeedKmh 220` o `#aria set skill.USSR.skill 0.8`. Si el valor no es válido, se rechaza. Para cambiar el preset hay que editar el archivo y usar `#aria reload`. Apagar `squad.enabled`, `vehicle.enabled` o `heli.enabled` en caliente devuelve la IA al juego vanilla de forma limpia; los helicópteros aterrizan antes |
| `#aria save` | Guarda la configuración actual en el archivo, con **todos** los valores activos. A partir de ahí el preset ya no los cambia |

### Ajustes principales
| Sección | Ejemplos |
|---|---|
| `general.*` | `enabled`, `preset`, `logLevel`, `excludedFactions`, `maxGroupsPerFrame` |
| `squad.*` | `shareIntel`, `intelShareRadius`, `flanking`, `suppressAndHold`, `withdraw*`, `reinforcements`, `useStaticWeapons`, `useNearbyVehicles` |
| `vehicle.*` | `fillTurrets`, `dismountOnContact`, `dismountDistance`, `crewSupportsDismounts`, `stuckRecovery`, `flipRecovery`, `bailOutHealth`, `convoyMaxSpeedKmh` |
| `heli.*` | `cruiseSpeedKmh`, `cruiseAltitude`, `combatAltitude`, `maxTiltDeg`, `lz*`, `cas*`, `autoAirlift`, `autoCAS`, `rtbHealth`, `copilotTakeover` |
| `heli.ctl.*` | Ajuste fino del piloto automático: `flightModel` (0 = velocidad, 1 = fuerzas), ganancias |
| `skill.<FACCIÓN>.*` | `skill`, `perception`, `reactionTime`, `courage`, `aggression`, `teamwork` |

---

## Arquitectura

```
SCR_BaseGameMode (modded) ──► ARIA_Manager (servidor, cada frame)
                                 ├─ ARIA_GroupBrain   × escuadra  → actividades vanilla (Move, S&D, GetIn/Out)
                                 │     ├─ ARIA_ThreatMemory  (contactos)
                                 │     └─ ARIA_TacticsPlanner (flanco / retirada / refuerzo con línea de visión)
                                 ├─ ARIA_VehicleBrain × vehículo  → torretas, desembarco, rescate, convoy
                                 ├─ ARIA_HeliPilot    × helicóptero → tareas → ARIA_FlightController → ARIA_HeliAirframe
                                 ├─ ARIA_Comms   (radio: inteligencia y peticiones de ayuda)
                                 └─ ARIA_AirOps  (aerotransporte y CAS bajo demanda)
```

| Carpeta | Contenido |
|---|---|
| `Scripts/Game/ARIA/Core` | Gestor, configuración (`ARIA_Settings`, `ARIA_Config`), matemáticas y PID, consultas del mundo, arranque |
| `Scripts/Game/ARIA/Squad` | Cerebro de escuadra, memoria de amenazas, planificador táctico y radio |
| `Scripts/Game/ARIA/Vehicles` | Clasificación de vehículos, compartimentos y cerebro de vehículo terrestre |
| `Scripts/Game/ARIA/Aviation` | Controlador de vuelo, adaptador del helicóptero, buscador de LZ, piloto, traductor de waypoints y AirOps |
| `Scripts/Game/ARIA/Api` | API pública y comando `#aria` |

**Cómo vuela.** Reforger 1.8 no expone al script los mandos de colectivo ni de cíclico, y
`AIHelicopterMovementComponent` está vacío. ARIA hace lo mismo que el código cinemático de
Bohemia:

- mantiene viva la simulación nativa (motor, RPM del rotor, sonido, daños);
- controla el cuerpo físico en el servidor, con la velocidad (modelo 0) o con impulsos de empuje (modelo 1);
- gobierna la actitud con velocidad angular, de modo que el helicóptero cabecea y se inclina de forma realista.

Toda la interacción con el motor del helicóptero está en `ARIA_HeliAirframe.c`.

---

## Plan de pruebas en el juego

1. **Arranque**: en el log aparece `[ARIA] ARIA started` y se crea `ARIA_Settings.cfg`.
2. **Inteligencia por radio**: coloca dos escuadras amigas a 500 m y un enemigo visible solo para una de ellas. La otra debería reaccionar unos segundos después. Con `logLevel = 3` verás `reports contact`.
3. **Flanqueo y retirada**: con `squad.flanking = true` y `skill.default.aggression = 1`, la escuadra fija al enemigo y luego lo rodea. Para probar la retirada, ponle mucha desventaja numérica.
4. **Vehículos**:
   - Escuadra con waypoint a más de 700 m y un camión vacío cerca: debe montarse.
   - Pon un enemigo en la ruta: el transporte debe desembarcar a la infantería.
   - Vuelca un vehículo: debe enderezarse a los 6 s.
5. **Helicóptero**:
   - UH-1H con tripulación de IA y un waypoint Move a 2 km: arranque, despegue, crucero y estacionario.
   - Añade un Get Out con tropas a bordo: debe aterrizar y desembarcarlas.
   - Helicóptero sin waypoints en base y una escuadra con objetivo a 3 km: debe hacer el aerotransporte.
6. **Rendimiento**: `#aria status` con 50 o más escuadras. Ajusta `general.maxGroupsPerFrame` según haga falta.

Si el vuelo se ve brusco u oscila, ajusta `heli.ctl.attitudeGain` y
`heli.ctl.angularResponse`. Si el helicóptero gira o se ladea **al revés**, la convención de
velocidad angular del motor es la inversa de la esperada: avísame y se corrige con un cambio
de signo en `ARIA_FlightController`.

---

## Herramientas de desarrollo

```bash
# Verificador estático contra los scripts oficiales (clona Arma-Reforger-Script-Diff)
python3 tools/aria_lint.py <ruta>/Arma-Reforger-Script-Diff/scripts Scripts

# Banco de pruebas del piloto automático (port 1:1 del controlador)
python3 tools/flight_sim.py --model velocity
python3 tools/flight_sim.py --model forces

# Regenerar docs/ARIA_Settings.example.cfg desde ARIA_Settings.c
python3 tools/gen_example_cfg.py
```

---

## Limitaciones conocidas y hoja de ruta
- **No compilado ni probado en el juego todavía** (ver arriba).
- Los helicópteros atacan con los **artilleros de puerta**. El piloto aún no dispara cohetes ni armas frontales, así que las pasadas de ataque con cohetes quedan para una versión futura.
- No hay aviones de ala fija.
- El desembarco se hace aterrizando. No hay *fast-rope*, porque Reforger no lo tiene.
- Los waypoints asignados a un helicóptero desaparecen del mapa del Game Master.
- Con `#aria set heli.enabled false` en caliente, los helicópteros en vuelo aterrizan donde pueden y luego devuelven el control al juego.
- Futuro: menú de órdenes para el Game Master, uso de artillería y morteros, guarniciones en edificios y pasadas de ataque con cohetes.

## Licencia
Aún no se ha definido. Antes de publicar en el Workshop, elige una; en los mods de Arma es
habitual la [APL-SA](https://www.bohemia.net/community/licenses/arma-and-dayz-public-license-share-alike).
ARIA usa la API pública de scripting de Arma Reforger. Arma Reforger y Enfusion son marcas de
Bohemia Interactive a.s.
