# dlss5-gfn-overlay

Applica **DLSS 5 Neural Rendering in tempo reale** alla finestra di GeForce NOW
(o a qualunque altra finestra), con un tasto per accendere e spegnere l'effetto
e confrontare al volo.

Non è affiliato a NVIDIA, ReShade o RenoDX.

## Cosa fa, e cosa non fa

Il pass di Neural Rendering di DLSS 5 (`nvngx_dlssnr.dll`, feature 18) lavora
**sul colore**: prende un frame e ne restituisce uno migliorato, con controlli
di tono locale, struttura e struttura della pelle. Non gli servono depth buffer
né motion vector veri, ed è per questo che si può applicare a un video già
decodificato. Il progetto [dlss5-visual-enhancer][enhancer] lo fa su file; qui
lo stesso pass viene applicato a una finestra viva, frame per frame.

Quello che **non** succede: nessun DLSS Super Resolution e nessun Frame
Generation sul gioco. Quelli hanno bisogno dei buffer del motore, che sul tuo PC
non esistono perché il gioco gira sui server NVIDIA. Se vuoi il DLSS vero su
GeForce NOW, lo attivi dentro le impostazioni grafiche del gioco: su GFN
Ultimate gira lato server sulle macchine RTX 5080.

Questa app serve per tutto il resto: i giochi che DLSS 5 non ce l'hanno, e in
generale qualunque finestra tu voglia migliorare.

## Prova prima le due strade che non richiedono questa app

Il pacchetto ReShade del DLSS 5 (`dxgi.dll` di ReShade, `renodx-dlss5.addon64`,
`dlss5-feed.addon64`, gli ini e i runtime NVIDIA) va messo **accanto
all'eseguibile dell'applicazione da migliorare**: l'add-on gira dentro quel
processo e lavora sulla sua swapchain. È così che funziona su OBS, che è
esattamente lo stesso caso di GeForce NOW, cioè un'app D3D che disegna un
video senza geometria.

1. **Copia il pacchetto accanto al client GeForce NOW.** Se parte, hai finito:
   latenza aggiunta zero oltre al costo del pass neurale, nessuna copia, nessun
   processo in mezzo. Non esiste niente di meglio di questo.
2. **Se il client rifiuta di partire con una DLL proxy accanto**, prova
   l'injector globale di ReShade, che aggancia il processo a runtime invece di
   passare dall'ordine di ricerca delle DLL.
3. **Se nemmeno quello passa**, questa app fa da ospite: cattura la finestra di
   GeForce NOW, la presenta nella propria swapchain, e ReShade migliora quella.
   Costa una copia sulla GPU in più, e niente altro.

Il terzo caso è l'unico in cui serve compilare qualcosa.

## Come è fatto

Due modalità, scelte con `enhance_mode`.

### `reshade` (predefinita)

```
finestra sorgente
   │  cattura, resta sulla GPU
   ▼
pass shader: crop + BGRA→RGBA ──► swapchain di questa app
                                        │
                                        ▼  ReShade + add-on DLSS 5,
                                           agganciati alla nostra swapchain
                                        ▼
                                     schermo
```

Niente lascia mai la GPU. L'app fa solo due cose: cattura e presenta. Il costo
aggiunto è una copia di texture più il pass neurale, che sarebbe da pagare
comunque.

Per farla funzionare, il pacchetto ReShade va copiato **accanto a
`dlss5-gfn-overlay.exe`**. All'avvio l'app controlla se un `dxgi.dll` proxy
della propria cartella è stato caricato al posto di quello di sistema, e lo
dice: senza quel controllo non avresti modo di distinguere un frame migliorato
da uno intatto.

In questa modalità l'accensione e lo spegnimento dell'effetto appartengono a
ReShade, con il suo tasto configurato in `ReShade.ini`. L'app non registra un
proprio toggle che non farebbe niente.

### `worker`

```
cattura ──► render target RGBA8 ──► readback su CPU
                                          │
                                          ▼
                              feeder DLSS in un processo separato
                              stdin:  header + RGBA + motion
                              stdout: header + RGBA migliorato
                                          │
                                          ▼  upload su GPU, poi swapchain
```

Serve un worker che parli il protocollo `--video`, cioè un `nvngx.dll` costruito
dai sorgenti del feeder. I pacchetti ReShade non lo contengono: hanno l'add-on,
che gira in-process. Questa modalità esiste per chi quel worker ce l'ha.

In entrambe le modalità la cattura tiene solo l'ultimo frame e butta i
precedenti: su uno stream cloud la latenza conta più della fluidità. In modalità
`worker` il feeder gira in un processo separato anche perché la feature 18 su
certe combinazioni driver/GPU muore con `0xC0000005`, e così muore il worker
invece dell'overlay.

## Il conto della latenza

In modalità `reshade` non c'è quasi niente da contare: una copia di texture
sulla GPU e il pass neurale. Sopra ci resta la latenza dello stream cloud,
30-60 ms, che c'era comunque.

In modalità `worker` invece ogni frame attraversa le pipe, e a 1920×1080 sono
8,3 MB di RGBA in ingresso, 8,3 MB di campo motion e 8,3 MB di RGBA in uscita:
~25 MB per frame, cioè 1,5 GB/s a 60 fps, più il viaggio GPU→CPU→GPU. A 1440p
sono 44 MB per frame. Il campo motion è la voce più stupida della lista, lo
riempiamo di zeri e lo mandiamo lo stesso perché il protocollo lo prevede. È il
motivo per cui `reshade` è la modalità predefinita.

Non ti do una stima inventata di quanti millisecondi costa sul tuo PC. C'è
`--bench` apposta:

```
dlss5-gfn-overlay.exe --bench 300
```

Processa 300 frame e stampa mediana e 95° percentile di ogni stadio, più la
latenza vera end-to-end dalla cattura alla presentazione. Se il totale supera i
16,6 ms non tieni i 60 fps, e in modalità `worker` il primo intervento è
abbassare `neural_input_scale`.

## Requisiti

- Windows 10 1903 o successivo (Windows 11 per togliere il bordo giallo della
  cattura), 64 bit, Direct3D 11.
- GPU NVIDIA RTX. Ada e Blackwell (RTX 40/50) sono i bersagli principali del
  runtime.
- I file di runtime NVIDIA, ReShade e RenoDX, che **non sono in questo repo**.
  Vedi [`runtime/BINARIES.md`](runtime/BINARIES.md).

### Su RTX 30 (Ampere)

Il Neural Rendering su Ampere gira sul percorso sperimentale ed è nettamente
più lento che su RTX 40/50. Funziona, ma il budget per frame è il problema, e
la configurazione di default quasi certamente non tiene i 60 fps.

Parti da qui invece che dai default:

```ini
neural_input_scale = 0.667
upscale_factor = 1.5
```

Il runtime lavora su 1280x720 e restituisce 1920x1080: meno della metà dei
pixel da elaborare rispetto al DLAA nativo, e altrettanti byte in meno da far
passare nelle pipe. Poi misura con `--bench 300` e alza o abbassa
`neural_input_scale` finchè il totale non sta sotto i 16,6 ms.

Frame generation su Ampere non è disponibile: serve Ada o Blackwell.

## Compilare

### Da Linux, senza Visual Studio

```bash
sudo apt install g++-mingw-w64-x86-64
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

Esce un `dlss5-gfn-overlay.exe` che dipende solo da DLL di sistema Windows:
niente runtime MinGW da distribuire accanto.

### Con Visual Studio

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Servono solo Windows SDK e MSVC, niente vcpkg.

### I due backend di cattura

| | `wgc` | `dxgi` |
| --- | --- | --- |
| API | Windows Graphics Capture | Desktop Duplication |
| Cattura | solo la finestra bersaglio | il monitor intero, poi ritaglia |
| Segue la finestra fra monitor | sì | no |
| Toolchain | solo MSVC (serve C++/WinRT) | qualunque, MinGW compreso |
| Requisiti | Windows 10 1903 | Windows 8, ma vedi sotto |

`auto` sceglie `wgc` con MSVC e `dxgi` altrimenti. Per forzare:
`-DGFN_CAPTURE_BACKEND=dxgi`.

Il backend `dxgi` duplica il monitor, quindi filmerebbe se stesso. La finestra
di output si marca `WDA_EXCLUDEFROMCAPTURE` e sparisce da qualunque cattura
schermo: serve Windows 10 2004, e sotto quella versione l'app avvisa invece di
lasciarti scoprire il tunnel infinito di overlay da solo. Lo stesso flag rende
l'overlay invisibile anche a OBS e agli screenshot; si spegne con
`exclude_from_capture = false`, ma allora usa un monitor diverso per l'output.

## Usare

1. Metti i file di runtime come descritto in `runtime/BINARIES.md`.
2. Copia `config.example.ini` in `config.ini` accanto all'eseguibile e aggiusta
   `window_title`. Per vedere i titoli disponibili: `--list-windows`.
3. Avvia GeForce NOW, poi l'overlay. Se lo avvii prima, aspetta la finestra.
4. `alt+F1` accende e spegne il Neural Rendering, `alt+F2` stampa le latenze,
   `alt+F4` esce.

In passthrough l'overlay presenta il frame catturato direttamente dalla GPU,
senza giro sul worker: il confronto con l'effetto acceso è quindi onesto, stesso
percorso di presentazione, stessa scala.

Opzioni utili:

```
--window "GeForce NOW"   sovrascrive il titolo dal config
--bench 300              misura e esce
--stats 5                stampa le latenze ogni 5 secondi
--log-level debug        più dettagli, compreso lo stderr del worker
```

## Stato: compila e linka, mai eseguito

Compila pulito con `-Wall -Wextra` su MinGW-w64 13 e linka in un eseguibile
Windows x86-64 autonomo. Non è **mai stato eseguito**: qui non c'è una macchina
Windows, tantomeno una con una RTX e i runtime NVIDIA.

Compilare ha già ripagato: ha trovato un `std::ifstream` aperto con una
`std::wstring`, che è un'estensione MSVC e su libstdc++ non esiste, e una
struct privata usata da fuori della classe. Due bug veri che una rilettura non
aveva visto.

Il protocollo del worker non è indovinato: è stato letto dai sorgenti di
[dlss5-visual-enhancer][enhancer] e i `static_assert` sulle dimensioni delle
struct falliscono a compile time se il layout non torna.

Quello che resta da verificare sull'hardware vero:

- che il worker regga una sessione continua con `frame_count = 0` invece di un
  conteggio noto. Se rifiuta il setup, `frame_count_hint` nel config accetta un
  numero grande.
- quanto costa davvero un frame. È la domanda a cui risponde `--bench`.
- che Desktop Duplication non venga rifiutata dal client GeForce NOW se gira in
  full screen esclusivo. In quel caso passa il client a finestra senza bordi.
- se il crop di default va bene, o se serve tagliare della chrome.

## Rischi da conoscere

**Termini di servizio.** L'overlay cattura la finestra dall'esterno con
Windows Graphics Capture, non inietta niente nel client GeForce NOW. È più
prudente dell'injection, ma resta un'area grigia: valutala tu.

**Anti-cheat.** Questa app non tocca il processo del gioco. Vale comunque la
regola generale: non usare mod di questo tipo con giochi online competitivi.

**Contenuto protetto.** Se una finestra usa una superficie protetta da DRM, la
cattura restituisce nero. È un limite di Windows, non un bug qui.

## Prossimi passi, in ordine di resa

1. **Abbassare `neural_input_scale`.** È la leva più forte e l'unica che
   agisce su tutti i costi insieme: meno pixel da elaborare, meno byte da
   leggere dalla GPU, meno byte nelle pipe. Con `neural_input_scale = 0.667` e
   `upscale_factor = 1.5` il runtime lavora su 720p e restituisce 1080p.
2. **Sovrapporre cattura e round trip.** Oggi il thread neurale fa cattura,
   readback e giro sul worker in sequenza. Facendo il readback del frame
   successivo mentre il worker lavora sul precedente si nascondono i 2-5 ms
   della cattura dietro i millisecondi del pass neurale. Guadagno reale ma
   modesto: vale la pena solo dopo che `--bench` dice che il resto è a posto.
3. **Frame generation** con `nvngx_dlssg.dll`. Non è disponibile su Ampere:
   serve Ada o Blackwell. Aggiunge comunque un frame di buffer, quindi su cloud
   gaming va misurato prima di deciderlo.

Due ottimizzazioni che sembrano ovvie e non sono possibili: **memoria condivisa
al posto delle pipe** e **non mandare il campo motion quando è tutto zero**.
Entrambe richiedono di cambiare il protocollo del worker, e del worker esiste
solo il binario: i sorgenti nativi del feeder non sono pubblicati. Restano sul
tavolo solo se scrivi un feeder tuo.

## Licenza

Codice originale sotto MIT. La licenza copre solo questo codice: non
ridistribuisce né concede diritti su runtime NVIDIA, RenoDX, ReShade o altri
binari di terze parti. NVIDIA, GeForce RTX, NGX e DLSS sono marchi di NVIDIA
Corporation.

[enhancer]: https://github.com/Merserk/dlss5-visual-enhancer
