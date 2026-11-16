# Voice Denoise: integrazione open source reale

## Componente incluso

Sculpt Clean Studio usa **RNNoise compatto v0.1** nel trattamento offline
della voce. Sono inclusi i sorgenti C originali e il modello neurale addestrato
`rnn_data.c`; non è un nome applicato al precedente gate o denoiser spettrale.

Il repository ufficiale è [xiph/rnnoise](https://github.com/xiph/rnnoise).
La revisione fissata è `cdf196b1e9de2f8ff1003328ebf9a4316477429d`, tag `v0.1`.
I file originali sono conservati senza modifiche in `ThirdParty/RNNoise`,
insieme a `COPYING`, attribuzioni e manifest SHA-256 `upstream.json`.
I termini BSD del progetto sono inclusi integralmente. Il modello è quello
compatto storico; non viene dichiarato come RNNoise 0.2 o come modello più recente.

Il metodo combina una rete ricorrente per la stima dei guadagni con analisi
spettrale e filtraggio del pitch. Il riferimento primario è Jean-Marc Valin,
[A Hybrid DSP/Deep Learning Approach to Real-Time Full-Band Speech Enhancement](https://jmvalin.ca/papers/rnnoise_mmsp2018.pdf).
La qualità riportata nel paper non viene trasferita automaticamente alle prove
di Sculpt o confrontata con RX12 senza una valutazione dedicata.

## Uso e limiti

- Pensato per **parlato**, non per preservare un mix musicale completo.
- Non richiede un tratto di solo rumore e non usa il profilo Learn del VST3.
- L'intensità regola la miscela fra originale allineato e risultato RNNoise.
- Il trattamento viene renderizzato sul file dal worker dello Studio. Non
  aggiunge un'inferenza neurale continua a ogni insert del mix.
- La libreria nativa lavora a **48 kHz**: il chiamante deve ricampionare prima
  e dopo. L'adapter rifiuta esplicitamente le altre frequenze.
- Ogni canale stereo ha il proprio stato e la propria stima dei guadagni:
  l'immagine stereo può cambiare. La destinazione preferita è una voce mono.
- Non separa voci sovrapposte, dialogo da musica o strumenti da un brano.
- Non è un modulo Dialogue Isolate, Music Rebalance o un sostituto completo di RX.

Il profilo spettrale manuale rimane utile per rumori stazionari e strumenti.
La prova sintetica di RNNoise qui inclusa attenua il rumore bianco di circa
**3,94 dB**: non viene pubblicizzata una soppressione superiore che non è stata
osservata. L'efficacia su parlato reale, consonanti, respiri e rumore variabile
richiede ascolto su registrazioni rappresentative.

## Interfaccia del motore

`Source/VoiceDenoiser.h` espone `prepare(rate, channels)`, `setAmount(0…1)`,
`reset()`, `processSample(left, right, channels)`, `isReady()` e
`getLatencySamples()`. Per un render con intensità costante, impostare Amount
prima di `prepare()` o di `reset()`.

La latenza di riferimento è **960 campioni a 48 kHz**: 480 per la finestra
sovrapposta del motore e 480 per raccogliere un pacchetto nell'adapter streaming.
Per renderizzare N campioni, elaborare N+960 campioni aggiungendo silenzio in
coda, scartare i primi 960 e mantenere N campioni. Lo Studio deve effettuare
questa compensazione prima di reinserire il risultato nel documento.

La pianificazione FFT condivisa di RNNoise v0.1 viene inizializzata una sola
volta, in modo serializzato, durante `prepare()`. Il processo sui campioni non
contiene mutex o allocazioni C/C++ dell'adapter. I dati addestrati sono compilati
nel programma e non richiedono servizi remoti, Python o download all'avvio.

`cmake/RNNoise.cmake` definisce il target statico `SculptRNNoise`, da collegare
al solo Studio. `tools/test-voice.sh` compila i sei sorgenti C originali e il
test C++ reale, senza sostituire RNNoise con un mock.

## Verifiche eseguite

- Frequenze non supportate rifiutate, inclusi valori non finiti.
- Amount zero: originale ritardato esattamente di 960 campioni, anche stereo.
- Impulso trattato: picco principale allineato a ingresso +960 campioni.
- Confronto con chiamate dirette a `rnnoise_process_frame`: errore massimo
  osservato **7,45 × 10⁻⁹** nell'adapter.
- Reset e render ripetuti deterministici; output finito per NaN e infinito in ingresso.
- Controllo delle allocazioni C++ e, su Linux, di malloc/calloc/realloc dopo prepare.
- AddressSanitizer e UndefinedBehaviorSanitizer superati sui sei sorgenti C
  originali e sull'adapter. LeakSanitizer disattivato nell'ambiente sotto ptrace;
  questa esecuzione non viene presentata come verifica delle perdite di memoria.

Il test riporta anche il tempo di rendering di quattro secondi mono. Le esecuzioni
locali osservate hanno richiesto circa **0,10–0,26 s su Linux**: è un dato del motore compatto,
non una misura della DAW su Mac né una certificazione del numero di istanze.

## Altri progetti valutati

| Progetto | Utilità e decisione |
|---|---|
| [RNNoise attuale](https://github.com/xiph/rnnoise/releases) | La versione 0.2 migliora addestramento e ottimizzazioni. Questa integrazione usa volutamente il modello compatto v0.1, già incluso nei sorgenti fissati. |
| [libspecbleach](https://github.com/lucianodato/libspecbleach) | Libreria C LGPL-2.1 per denoise spettrale, stima adattiva e mascheramento. La modalità NLM in due dimensioni è dichiarata onerosa e usa un pool di worker: non viene aggiunta automaticamente agli insert leggeri. Non integrata in questa modifica. |
| [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) | Framework di miglioramento del parlato a 48 kHz con implementazioni Rust/Python e modelli addestrati. È una possibile estensione offline più complessa; non è presente nel binario consegnato da questa integrazione. |

Questa scelta aggiunge una vera elaborazione neurale open source, ma non
dimostra da sola la completezza o la qualità complessiva di RX12.
