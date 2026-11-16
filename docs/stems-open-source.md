# Separazione musicale e rebalance offline: Open-Unmix UMX-HQ

Sculpt Clean Studio può separare **voce, batteria, basso e altri strumenti** con i quattro modelli realmente addestrati di Open-Unmix UMX-HQ. Il rebalance regola poi il livello dei quattro WAV. La voce isolata si ottiene mantenendo `vocals` a 0 dB e portando gli altri tre gruppi a −60 dB, valore che in questo strumento significa mute.

Questa funzione lavora in un processo locale separato. Non invia l'audio a un servizio esterno e non viene inserita nel callback audio del VST3. Il processo termina dopo l'elaborazione: non rimane un carico neurale continuo nella DAW.

**La separazione neurale non è leggerissima durante il render.** Due thread limitano il parallelismo, non eliminano il costo dei quattro modelli. La qualità non è stata confrontata con RX e non si promettono isolamento perfetto, assenza di artefatti, separazione di qualsiasi sorgente, Dialogue Isolate, Ambience Match o equivalenza a RX. UMX-HQ nasce per le quattro categorie musicali indicate; sovrapposizioni e residui sono possibili.

## Codice, modelli e licenze verificabili

- Codice: [sigsep/open-unmix-pytorch, release v1.3.0](https://github.com/sigsep/open-unmix-pytorch/tree/v1.3.0), licenza MIT. Copia della licenza in `ThirdParty/OpenUnmix/LICENSE`.
- Pesi: [Open-Unmix-Pytorch UMX-HQ, Zenodo 3370489](https://zenodo.org/records/3370489), DOI [10.5281/zenodo.3370489](https://doi.org/10.5281/zenodo.3370489), Fabian-Robert Stöter e Antoine Liutkus.
- La [metadata API ufficiale del record](https://zenodo.org/api/records/3370489) dichiara espressamente `license.id: mit-license`. La porzione rilevante della risposta è conservata in `ThirdParty/OpenUnmix/UMXHQ-UPSTREAM-METADATA.json`. La scelta è **UMX-HQ**, non UMX-L: la licenza non commerciale di UMX-L non deve essere attribuita erroneamente a questi pesi.
- Panoramica e caratteristiche del progetto: [Open-Unmix ufficiale](https://sigsep.github.io/open-unmix/).
- L'integrazione non copia DSP proprietario iZotope né dipende da RX.

I quattro checkpoint ufficiali sono stati effettivamente scaricati per lo sviluppo, controllati contro gli MD5 pubblicati da Zenodo e poi registrati con SHA256 completo. Non sono inseriti nel repository: il setup li scarica e controlla automaticamente. Ogni file pesa 35.637.796 byte; totale dei pesi **142.551.184 byte** (circa 142,6 MB decimali).

| Modello | SHA256 del checkpoint ufficiale |
| --- | --- |
| bass-8d85a5bd.pth | `8d85a5bd3f996a8867fca0e8442e077e5a3f5ec747a6112742452a8f347b39c8` |
| drums-9619578f.pth | `9619578f885c54737cb0234f9f9a4a679ee4f31438fd77fd1dbe02bb16c2da0a` |
| vocals-b62c91ce.pth | `b62c91cedbc7a066f1778ead5b5cecb377aa3a46a31af1cce7c5c8769339d083` |
| other-b52fbbf7.pth | `b52fbbf76479e752bd72e02304c602ac7802aa5bfbfb9cd12054b2695d5093ab` |

URL, dimensioni, MD5 originali e SHA256 completi sono anche in `tools/stems-models.json`. Prima di caricare un checkpoint, il worker verifica il suo SHA256 e usa `torch.load(..., weights_only=True)`. I tre buffer di analisi storici `sample_rate`, `stft.window`, `transform.0.window`, trasferiti nella classe `Separator` dalle versioni recenti di Open-Unmix, vengono ignorati esplicitamente; tutti i tensori appresi sono caricati con `strict=True`.

## Installazione privata su macOS

Da Studio si può avviare il setup. Da Terminale, nella cartella del progetto:

```bash
bash tools/setup-stems.sh
```

Cartella predefinita:

```text
~/Library/Application Support/SculptClean/stems/
    venv/bin/python3
    models/
    python/             # presente soltanto se serve il runtime privato
```

Non sono richiesti Homebrew o privilegi di amministratore. Il setup usa un Python 3.10–3.12 compatibile già disponibile per creare l'ambiente privato. Se manca, su macOS scarica CPython 3.11.11 dal progetto [python-build-standalone](https://github.com/astral-sh/python-build-standalone/releases/tag/20241206), separatamente per Apple Silicon e Intel, e verifica un SHA256 fissato nel codice. Non modifica l'installazione Python di sistema.

| Archivio privato Python, release 20241206 | SHA256 pubblicato dal progetto |
| --- | --- |
| cpython-3.11.11+20241206-aarch64-apple-darwin-install_only.tar.gz | `566c5e266f2c933d0c0b213a75496bc6a090e493097802f809dbe21c75cd5d13` |
| cpython-3.11.11+20241206-x86_64-apple-darwin-install_only.tar.gz | `8ecd267281fb5b2464ddcd2de79622cfa7aff42e929b17989da2721ba39d4a5e` |

Gli hash sono stati verificati leggendo gli asset `.sha256` ufficiali della release. **Il bootstrap macOS non è stato eseguito in questo ambiente Linux.** La compatibilità finale dell'app su Mac richiede la prova del pacchetto su hardware Apple Silicon e Intel. Le distribuzioni Python mantengono le proprie licenze e quelle dei componenti inclusi; il progetto di build è MPL-2.0 e CPython ha la licenza PSF. Il setup scarica gli archivi originali e ne conserva il contenuto, incluse le notifiche.

Le dipendenze dirette e transitive sono fissate in `requirements-stems.txt`, con gli SHA256 delle wheel pubblicate su PyPI. Il setup usa `--require-hashes --only-binary=:all: --no-deps`. Versioni principali: Open-Unmix 1.3.0, PyTorch/torchaudio 2.2.2, NumPy 1.26.4, SoundFile 0.12.1, certifi 2026.10.23. Le wheel selezionate di Torch/torchaudio 2.2.2 per Apple Silicon hanno tag macOS 11.0; le varianti Intel hanno tag 10.9/10.13. La versione Torch 2.2.2 è una baseline comune per Mac Intel e Apple Silicon, non una dichiarazione che sia la release più recente. SciPy non è necessario: il ricampionamento usa torchaudio, evitando la dipendenza dalle wheel SciPy ARM che richiedono macOS 12. Il download dei modelli aggiunge i certificati Mozilla di certifi al contesto TLS verificato: non dipende dall’avere eseguito Install Certificates.command su un Python esistente e non disabilita la verifica HTTPS. Su Linux x86_64 usa wheel CPU verificate separatamente in `stems-linux-wheels.json`, evitando le dipendenze CUDA.

Il download iniziale richiede spazio anche per Python, PyTorch e le altre librerie: **142,6 MB è soltanto la dimensione dei pesi**, non dell'intera installazione. Installazione e download richiedono Internet; le elaborazioni successive funzionano localmente senza Internet.

## Contratto CLI per l'app

```bash
bash setup-stems.sh "/percorso/installazione" "/percorso/cancel-setup"
"/percorso/installazione/venv/bin/python3" sculpt-stems.py status --model-dir "/percorso/installazione/models"
"/percorso/installazione/venv/bin/python3" sculpt-stems.py separate input.wav /nuova/cartella/stems --model-dir "/percorso/installazione/models" --threads 2 --cancel-file /percorso/cancel-job
"/percorso/installazione/venv/bin/python3" sculpt-stems.py rebalance /cartella/stems nuovo-mix.wav --vocals-db 0 --drums-db -3 --bass-db 1 --other-db -2 --cancel-file /percorso/cancel-job
```

`setup-stems.sh` accetta due argomenti posizionali opzionali: cartella di installazione e file di annullamento. Lo stdout finale contiene il JSON di `status`; i log del setup vanno su stderr. L'app deve leggere `ready`, non limitarsi alla presenza dello script. `status --require-ready` restituisce un exit code non nullo se manca una dipendenza o un modello verificato.

`separate` crea esclusivamente una cartella nuova con `vocals.wav`, `drums.wav`, `bass.wav`, `other.wav` e `stems.json`. `rebalance` richiede un nome WAV nuovo. Risultati JSON su stdout, progressi JSON su stderr; un errore produce `ok:false` e exit code 1. Il worker non scarica modelli implicitamente durante la separazione.

L'app annulla creando il file passato con `--cancel-file`; non deve cancellarlo prima dell'uscita del processo. Il worker controlla l'annullamento fra i blocchi e prima della pubblicazione. Un singolo blocco neurale già iniziato deve terminare prima di recepire l'annullamento. Il setup controlla il file mentre pip/curl/Python lavorano, termina il figlio diretto e rimuove il lock; `SIGKILL` non consente ad alcun programma di eseguire cleanup, quindi l'app deve preferire il file di annullamento. Un setup interrotto può lasciare dipendenze parziali, che una nuova esecuzione completa; un lock rimasto dopo chiusura forzata richiede verifica che non esista ancora un setup attivo prima della rimozione.

## Elaborazione e integrità del file

- Input mono/stereo; WAV e altri formati leggibili da SoundFile. Frequenze ammesse 8–192 kHz.
- Conversione offline a 44,1 kHz con ricampionamento sinc limitato in banda e finestra Kaiser, `torchaudio.functional.resample` (64 attraversamenti dello zero, rolloff 0,94759, beta 14,76966); riconversione alla frequenza originale e lunghezza originale.
- Rete UMX-HQ stereo, un'iterazione Wiener, CPU, massimo due thread. Un ingresso mono viene duplicato per il modello e ricondotto a mono in uscita.
- Blocchi centrali di 4 secondi, 1 secondo di contesto per lato e sovrapposizione sfumata di 0,1 secondi. Questo limita la memoria temporanea della rete; l'audio e gli stem completi rimangono comunque in RAM. File molto lunghi possono richiedere molta memoria: questa versione non è uno streaming su disco illimitato.
- Le differenze residue di ricostruzione sono aggiunte a `other`, documentate nel manifest come `mixture_consistency: residual_to_other`, affinché i quattro stem a 0 dB ricostruiscano il mix di partenza entro l'arrotondamento float.
- Nessuna normalizzazione indipendente degli stem e nessun limiter nascosto. WAV float32 preserva l'headroom; il rebalance restituisce anche il picco. Guadagni positivi possono produrre valori superiori a 0 dBFS.
- Hash degli stem in `stems.json`; il rebalance rifiuta file cambiati, percorsi fuori dalla cartella, dimensioni/frequenze diverse, NaN/Inf e guadagni fuori da −60…+12 dB.
- Pubblicazione atomica della cartella completa e del WAV di rebalance, con rinomina esclusiva (`renamex_np(RENAME_EXCL)` su Mac, `renameat2(RENAME_NOREPLACE)` su Linux). Un percorso di uscita già esistente viene rifiutato. L'audio originale non viene sovrascritto.

## Verifiche eseguite

Con runtime Linux x86_64, Python 3.12, PyTorch 2.2.2 CPU e i checkpoint ufficiali scaricati:

```bash
python Tests/stems_worker_test.py --model-dir /percorso/modelli
```

L'inferenza reale su un segnale sintetico stereo di 2 secondi a 48 kHz ha prodotto i quattro stem finiti, alla frequenza e lunghezza originali. Nell'ultima verifica, con il ricampionamento torchaudio e SciPy completamente disinstallato dal runtime di prova, il tempo interno incluso caricamento modelli era circa **1,4 secondi**, con due thread. L'errore massimo fra somma degli stem e input era **1,1 × 10⁻⁸**. La prova mono di 5 secondi, con attraversamento di un confine fra blocchi, ha richiesto circa **3,5 secondi**. I tempi variano sensibilmente con il carico del computer: questi numeri descrivono esclusivamente questo test Linux e non garantiscono elaborazione in tempo reale. Non sono un benchmark Mac o una misura della qualità musicale.

Sono verificati anche il mix a 0 dB, il mute per ottenere la sola voce, il rifiuto di output esistenti, la conservazione dell'originale, il rifiuto di stem modificati, dei guadagni non finiti e l'annullamento. Il test opzionale con modelli comprende inoltre un mono di 5 secondi per attraversare un confine fra blocchi. I test senza `--model-dir` dichiarano esplicitamente saltate le prove neurali; non sostituiscono l'inferenza con un mock.

Una verifica qualitativa richiede brani reali, ascolto degli stem e confronto con riferimenti: il test sintetico dimostra che il modello funziona e che l'integrazione conserva i file, non che separi bene ogni mix.
