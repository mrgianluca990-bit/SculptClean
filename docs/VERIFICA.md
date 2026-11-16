# Sculpt Clean 0.2.0 — verifica e limiti

Data: 16 novembre 2026. Sorgenti macOS, JUCE 8.0.4; VST3 più editor Studio.

## Che cosa è stato verificato

I motori sono stati compilati ed eseguiti su **Linux x86_64**: core VST3,
RNNoise con il modello incluso, port SPADE, strumenti offline, Cathar e
Open-Unmix UMX-HQ con i checkpoint ufficiali. I numeri qui sotto provengono da
segnali sintetici controllati e test di integrità. Non sono un confronto
percettivo con RX12 e non certificano la qualità su qualsiasi registrazione.

Il controllo sintattico di `Studio/Main.cpp` contro JUCE 8.0.4 è passato con
C++17 e `-Wall -Wextra -Wpedantic -Werror`. Sono stati riesaminati gestione del
worker, cancellazione, anteprima senza copie integrali, confini delle selezioni,
confronto con l'originale e protezione del percorso finale di esportazione.
Questo controllo del sorgente **non equivale all'esecuzione dell'app**.

Non sono stati eseguiti localmente build/link macOS, test nativo dell'app,
bootstrap Python su Mac, caricamento VST3 in una DAW, ascolto dell'uscita audio
o confronto con RX. Il workflow esegue i test nativi prima di produrre
`Sculpt-Clean-macOS.zip`; il loro esito rimane da osservare su GitHub Actions.

## Strumenti offline dello Studio

Test: `Tests/offline_tools_test.cpp`, compilato con i sei oggetti C RNNoise
reali e C++17 con avvisi trattati come errori. Il comando di suite è
`bash tools/test-studio.sh`; comprende anche Voice Denoise e OfflineRepair.

| Verifica controllata | Risultato osservato |
|---|---:|
| Tono 1 kHz, 44,1 → 48 → 44,1 kHz, errore interno rispetto all'originale | −110,514 dB |
| Impulso dopo lo stesso roundtrip | Spostamento 0 campioni |
| Tono 30 kHz, conversione 96 → 48 kHz | Rigetto alias −95,5634 dB |
| Core neutro, compensazione della latenza | Identità esatta per 1/47/2047/2048/2051/4117 campioni |
| Intensità zero: RNNoise, selezione spettrale, miscela della selezione | Identità esatta |
| Rettangolo 900–1100 Hz, profondità −18 dB, tono 1 kHz | −17,9979 dB |
| Stesso rettangolo: tono 6 kHz esterno | Variazione circa −0,00000043 dB |
| Campioni esterni all'intervallo temporale selezionato | Identici all'originale |
| Profilo appreso, Denoise 80% su rumore bianco | −16,9605 dB |
| RNNoise 25% dopo SRC, confronto con `dry + 0.25 * (wet − dry)` | Errore massimo 0 |

Passano inoltre conservazione del rapporto stereo/antifase, dissolvenza dei
bordi, controlli delle dimensioni, rifiuto del profilo a frequenza incompatibile,
selezione Learn troppo breve e cancellazione senza modificare il sorgente.
Le misure del ricampionamento escludono i bordi del file, dove manca il contesto
simmetrico; non descrivono la risposta completa del filtro a tutte le frequenze.
L'attenuazione spettrale modifica una selezione: non è una prova di risintesi
intelligente di materiale mancante.

### RNNoise compatto incluso

`Tests/voice_test.cpp` usa RNNoise v0.1 e i pesi C originali, non un mock.
Riferimento e provenienza: [voice-open-source.md](voice-open-source.md).

- Latenza dell'adapter: 960 campioni a 48 kHz, compensata nel render Studio.
- Confronto con chiamate dirette a `rnnoise_process_frame`: errore massimo
  7,45 × 10⁻⁹.
- Rumore bianco sintetico: attenuazione circa 3,94 dB.
- Amount zero esatto, impulso allineato, reset deterministico, gestione degli
  input non finiti e rifiuto delle frequenze diverse da 48 kHz nell'adapter.
- Controlli C++ e Linux malloc/calloc/realloc: nessuna allocazione dopo prepare
  nel percorso esaminato. ASan e UBSan passati sui sei sorgenti C e sull'adapter;
  LeakSanitizer disabilitato nell'ambiente sotto ptrace.

Quattro secondi mono hanno richiesto circa 0,10–0,26 s nelle esecuzioni Linux
osservate. È una misura del motore compatto, senza DAW o SRC. Il test non
misura consonanti, respiri, parlato in sovrapposizione o qualità musicale.
Ogni canale stereo ha il proprio stato; la conservazione dell'immagine stereo
su ogni sorgente non è garantita.

### De-clip e ricostruzione locale SPADE

`Tests/offline_repair_test.cpp` usa il port C++ effettivo del nucleo MIT
`andryr/spade-declipping`. Dettagli: [repair-open-source.md](repair-open-source.md).

| Fixture con originale noto | Miglioramento dell'errore quadratico |
|---|---:|
| Somma armonica, hard clip a ±0,38 | 13,4179 dB |
| Somma di frequenze modulata, hard clip a ±0,24 | 21,2802 dB |
| Intervallo danneggiato di 16 campioni | 20,5253 dB |
| Intervallo danneggiato di 64 campioni | 16,2773 dB |
| Intervallo danneggiato di 128 campioni | 23,4888 dB |

Campioni affidabili e materiale fuori selezione rimangono identici. Passano
antifase stereo, toni non tagliati, conservazione di un impulso breve voluto,
intensità zero, limiti del contesto e cancellazione prima/durante il render
senza risultati parziali. ASan e UBSan passati, con LeakSanitizer disabilitato.
Un fixture mono di 0,25 s ha richiesto circa 0,33–0,47 s su Linux.

Il materiale armonico favorisce l'ipotesi sparsa. Queste prove non dimostrano
il recupero di clipping morbido, limiter, parole mancanti o buchi lunghi.
Il massimo operativo di Repair, 4096 campioni, è distinto dalle selezioni
16–128 campioni su cui sono riportati i risultati favorevoli.

### Cathar: motori offline reali

L'eseguibile release Cathar 0.8.0 proviene dal sorgente fissato e incluso.
Otto fixture di integrazione sono passate; i test upstream della libreria
riportano **128 passati, 0 falliti, 1 ignorato** dall'autore (`spade_trace`).
Report: [cathar-linux-verification.json](cathar-linux-verification.json);
log upstream: [cathar-upstream-unit-tests.log](cathar-upstream-unit-tests.log).

| Fixture | Misura osservata |
|---|---:|
| De-hum adattivo: 50 Hz con armonica | Hum −52,27 dB; tono 997 Hz −0,00029 dB |
| Low cut 80 Hz | Rumble 30 Hz −34,08 dB; tono 997 Hz praticamente invariato |
| De-click AR, errore rispetto al segnale pulito | Miglioramento 91,79 dB |
| De-crackle, errore rispetto al segnale pulito | Miglioramento 19,83 dB |
| WPE, coda sintetica | −2,69 dB |
| **WPE, nota pulita 440 Hz desiderata** | **Attenuazione 23,53 dB: danno al segnale utile nel test** |
| De-ess multibanda | Burst 6 kHz −2,06 dB; tono 311 Hz praticamente invariato |
| EQ Match, stesso riferimento | Errore RMS relativo −138,88 dB |
| EQ Match, rapporto fra due toni | Target 10,45757 dB; risultato 10,45757 dB |
| Normalizzazione stereo | −23,0 LUFS; true peak −20,3 dBTP |

Le fixture sono di tre secondi a 16 kHz. WPE è provato con 12 tap, ritardo
2 frame e trattamento completo; lo Studio usa 10 tap, ritardo 3 frame e
miscela iniziale 20%. La misura −23,53 dB documenta un limite del metodo,
non il risultato del preset iniziale della UI. Le finestre WPE non vengono
ridimensionate automaticamente col sample rate.

Sono stati verificati anche output finiti, canali, conservazione dell'ingresso,
resampling 16→48 kHz, statistiche, chiamate De-plosive/De-rustle e Wow/Flutter.
Per questi ultimi controlli di invocazione non si dichiara una misura di qualità
su plosive, abiti o nastri reali. [offline-open-source.md](offline-open-source.md)
riporta opzioni, licenze delle dipendenze e limiti degli algoritmi.

### Open-Unmix UMX-HQ: inferenza reale, non prova di qualità musicale

I quattro checkpoint ufficiali UMX-HQ sono stati scaricati e verificati con
MD5/SHA256. Codice Open-Unmix e pesi UMX-HQ sono MIT; UMX-L non è utilizzato.
Il test opzionale `Tests/stems_worker_test.py --model-dir /percorso/modelli`
è stato eseguito con PyTorch 2.2.2 CPU, Python 3.12, Linux x86_64 e due thread.

| Verifica | Risultato osservato |
|---|---|
| Inferenza su stereo sintetico, 2 s a 48 kHz | Quattro stem finiti, lunghezza e frequenza originali |
| Tempo interno, caricamento dei modelli incluso, ultima verifica con torchaudio | Circa 1,4 s |
| Somma degli stem rispetto all'input | Errore massimo circa 1,1 × 10⁻⁸ |
| Mono di 5 s, oltre il confine fra blocchi | Elaborazione completata, circa 3,5 s |
| Rebalance e mute | Mix a 0 dB e sola voce verificati |
| Integrità | Output esistenti, stem modificati e guadagni non finiti rifiutati; originale conservato; annullamento verificato |

Questi tempi sono osservazioni dell’ultima esecuzione Linux e variano con il
carico; non sono una garanzia di render in tempo reale né un benchmark Mac.
Il runtime finale usa torchaudio per il ricampionamento e non richiede SciPy.

Il residuo viene assegnato ad Altro per la consistenza del mix; un piccolo
errore di somma **non misura** l'isolamento degli strumenti. Non è stato
misurato SDR/SI-SDR su un corpus musicale con stem di riferimento. La rete
usa quattro categorie musicali e non implementa separazione generale di
scene, dialogo, musica ed effetti. Setup, runtime, pesi e prove sono descritti
in [stems-open-source.md](stems-open-source.md).

## Verifiche native previste dal workflow

Il workflow `.github/workflows/build-macos-vst3.yml` compila Studio, helper
Cathar e VST3 per arm64 e x86_64, controlla le architetture e firma ad hoc.
Il runner esegue soltanto la propria architettura: entrambe le architetture
e le DAW di destinazione richiedono ancora prove di caricamento dedicate.

Il test JUCE `SculptCleanStateTests` controlla gli 11 parametri del VST3, i
1025 bin del profilo, salvataggi a trasporto fermo, Learn pendenti, valori non
validi, cambio di frequenza e bypass stereo allineato di 2051 campioni.
Il controllo sintattico del wrapper è stato eseguito su Linux; il test
collegato nativo non viene dichiarato come già passato.

Lo Studio include un percorso `--self-test` per controllare WAV float con
picco 1,25, confini e fine dell'anteprima tramite `AudioTransportSource`,
chiamata al Cathar incorporato, risorse Stems e immagine della UI a 1240×820.
È una verifica da eseguire nel workflow macOS; **non è stata eseguita nativamente
in questa consegna**. Non sostituisce una prova d'ascolto o la verifica dei
selettori di file sul Mac dell'utente.

I log attesi comprendono `core-tests-macos.log`, `studio-tests-macos.log`,
`plugin-state-macos.log`, `cathar-macos-verification.json`,
`studio-macos-smoke.log`, `studio-macos-smoke.png` e il benchmark macOS.
L'installazione opzionale dei modelli Stems è distinta dalla build e
dal test della presenza degli script nel bundle.

## Core VST3: risultati conservati dalla 0.1

Compilazione dei tre eseguibili standalone con C++17, `-O2 -Wall -Wextra
-Wpedantic -Werror`: superata. Comando riproducibile: `bash tools/test-core.sh`.

| Verifica controllata | Risultato osservato |
|---|---:|
| Denoise al massimo, rumore bianco stazionario appreso | −22,953 dB |
| Tono voluto dominante a 441 Hz: variazione di livello | +0,001373 dB |
| Tono più rumore: miglioramento dell'errore rispetto al tono pulito noto | 18,3704 dB |
| Overlap-add con guadagno spettrale uniforme −24 dB: errore assoluto massimo | 1,13 × 10⁻⁷ |
| Passaggi inattivo/attivo/bypass dello stadio spettrale su DC | Errore massimo 0 |
| Dereverb: coda esponenziale spettrale controllata | −11,4825 dB |
| Stesso test: variazione della parte diretta costante | 0,0000 dB |
| Segnale noto convoluto con stanza sintetica, Dereverb 75%: energia della coda tardiva | −4,0119 dB |
| Stessa stanza: livello degli attacchi diretti | −0,0733 dB |
| Stessa stanza: errore rispetto al segnale asciutto | Miglioramento 0,8025 dB |
| Armoniche sostenute asciutte, Dereverb 100%: livello | −0,0003 dB |
| Armoniche sostenute: errore della forma d'onda rispetto all'ingresso | −85,8767 dB |
| **Limite: nota asciutta a 440 Hz in decadimento, Dereverb 75%** | **Coda attenuata di 8,5571 dB** |
| De-hum 50 Hz, fondamentale e seconda armonica | −24,0071 e −24,0155 dB |
| De-hum 50 Hz: tono voluto a 997 Hz | −0,00112 dB |
| De-hum 60 Hz, fondamentale e seconda armonica | −24,0072 e −24,0155 dB |
| De-hum 60 Hz: tono voluto a 997 Hz | −0,00182 dB |
| De-click: impulsi isolati di uno e due campioni | Rimossi nel test |
| De-click al massimo: tono pulito a 997 Hz, compensato il ritardo | Errore massimo 0 |
| Catena completa: trattato più rimosso rispetto all'originale ritardato | Errore massimo 9,31 × 10⁻¹⁰ |

I test di dereverb usano sorgenti sintetiche note; la stanza con T60 0,6 s è
vicina al modello nominale del processore. Questi numeri non rappresentano
tutti gli ambienti e non sono un confronto con RX12 o con parlato reale.
Il test della nota in decadimento è mantenuto per rendere visibile un limite
del metodo, non per nasconderlo dietro la misura favorevole sulla stanza.

Ulteriori verifiche superate:

- Latenza esatta della catena di 2051 campioni e neutralità bit per bit a
  8, 44,1, 48, 96, 192 e 384 kHz, mono e stereo.
- Bypass allineato, con Source, trim e ascolto del rimosso abilitati: l'uscita
  dopo la transizione rimane l'originale ritardato senza applicare il trim.
- Indipendenza dalla suddivisione in blocchi, stereo in opposizione di fase
  e mantenimento del rapporto fra i canali.
- Apprendimento esplicito di due secondi, profilo fisso, richiamo dei bin e
  rifiuto dei profili non validi o incompatibili con la frequenza di campionamento.
- Zero allocazioni **C++** rilevate nel percorso audio, anche durante Learn e
  cambi dei parametri; questo controllo non equivale a uno strumento generale
  di tracciamento di tutte le allocazioni del sistema.
- Gestione di NaN, infinito, reset e buffer vuoti nei percorsi verificati.
- AddressSanitizer e UndefinedBehaviorSanitizer sui moduli spettrali. Il
  controllo delle perdite di LeakSanitizer era disattivato perché non supportato
  nell'ambiente sotto ptrace; non viene dichiarato come eseguito.
- Ottimizzazione di De-click: 500.000 campioni casuali, tonali, impulsivi e
  non finiti, con variazioni dei controlli, restituiscono gli stessi campioni
  bit per bit dell'algoritmo precedente basato sugli ordinamenti completi.

## CPU del core VST3: baseline conservata dalla 0.1

`tools/run-benchmark.sh` compila il motore in Release (`-O3 -DNDEBUG`) e produce
un JSON con metadati e tutti i risultati. Le condizioni sono:

- 48 kHz, stereo, 1 / 8 / 16 istanze indipendenti eseguite in serie;
- blocchi di 128 / 512 campioni;
- tre configurazioni: neutra, Denoise 55%, completa;
- completa: Denoise 55%, Dereverb 35%, De-click 45%, De-hum 50%, Source ON
  con Depth 40% e Threshold −30 dBFS;
- 256 callback di riscaldamento e 1536 callback misurati per condizione;
- stessi segnali e confini FFT sincronizzati fra le istanze, per esporre il
  picco simultaneo degli insert;
- generazione e copia del segnale fuori dall'intervallo cronometrato;
- controllo dei denormali coerente con il wrapper: FTZ/DAZ su x86 e FZ su
  AArch64, ripristinando il modo precedente dopo ogni callback;
- media, mediana, percentile 99, massimo, percentuale del budget e conteggio
  dei callback che superano il tempo disponibile, **senza scartare outlier**.

Il budget è 2666,67 µs a 128 campioni e 10666,67 µs a 512 campioni. La
percentuale del budget descrive il carico seriale rispetto a un intervallo
audio: non è la percentuale complessiva del Mac visualizzata dalla DAW.
Il benchmark non include GUI, wrapper APVTS, driver audio o scheduling della DAW.

Le misure locali sono su Linux x86_64, AMD EPYC 7763, con FFT C++ portabile.
Sono incluse la prima misura, una seconda senza gli altri test del progetto
contemporaneamente e la misura finale dopo l'ottimizzazione di De-click e
l'allineamento del controllo dei denormali al wrapper. Sono conservati anche
i dati del breve confronto dedicato ai denormali: non hanno spiegato da soli
i picchi osservati. La variabilità fra esecuzioni impedisce di attribuire
tutta la differenza di tempo a una singola modifica.

### Misura finale: elaborazione completa

| Istanze stereo | Blocco | Media | Percentile 99 | Massimo | Callback oltre il budget |
|---:|---:|---:|---:|---:|---:|
| 1 | 128 | 95,4 µs | 331,4 µs | 581,7 µs | 0 / 1536 |
| 16 | 128 | 1607,0 µs | 5317,7 µs | 30160,4 µs | 388 / 1536 |
| 1 | 512 | 397,5 µs | 825,0 µs | 5736,5 µs | 0 / 1536 |
| 16 | 512 | 7304,8 µs | 23690,0 µs | 58977,0 µs | 99 / 1536 |

La singola istanza completa usa in media circa il 3,6–3,7% dell'intervallo
audio nelle condizioni finali. Le 16 istanze sincronizzate superano il budget
in parte dei callback: questa misura **non certifica** la tenuta di 16 istanze
o un carico trascurabile. Anche le configurazioni Denoise e intermedie sono
riportate integralmente nel JSON, compresi i loro eventuali superamenti.

I JSON conservano tutti i valori, anche sfavorevoli. Il file nativo
`docs/cpu-macos-benchmark.json` sarà generato dal workflow utilizzando
Accelerate sul runner. Solo le successive prove nella DAW e sul Mac di
destinazione permetteranno di stabilire quante istanze sono sostenibili.

## Parti ancora mancanti e criteri di ascolto

La 0.2 aggiunge funzioni prima assenti; non completa ogni funzione o workflow
di RX. Mancano fra l'altro separazione avanzata di scene/dialogo/musica/effetti,
navigazione per trascrizione, ricostruzione spettrale estesa, moduli specializzati
validati per mouth click/respiri e ambiente, sessioni persistenti con più
file e una valutazione percettiva su materiale reale. L'attenuazione di un
rettangolo, il repair breve e UMX-HQ non sono equivalenti a tutte quelle funzioni.

Wow/Flutter opera sull'intero documento e può cambiare la durata: in questo
caso il confronto campione per campione con l'originale e il ripristino di una
selezione sono disabilitati. Annulla recupera la versione precedente.
Il test di invocazione del motore non certifica allineamento stereo, recupero
del trasporto di qualsiasi nastro o conservazione del vibrato intenzionale.

Per confrontare il trattamento con RX12 occorre usare gli stessi file,
allineare il volume e ottenere una riduzione simile. Valutare separatamente
rumore residuo, intelligibilità, consonanti, attacchi musicali, decadimenti,
immagine stereo e artefatti. Conservare gli originali e ascoltare il rimosso.
Nessuna prova comparativa con RX12 è stata eseguita in questa consegna.
