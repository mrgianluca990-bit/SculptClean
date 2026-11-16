# Open-Unmix dependency notice

Sculpt Clean Studio uses the unmodified Open-Unmix 1.3.0 Python package as an optional, separate offline process. Package and transitive wheel versions/hashes are pinned in `tools/requirements-stems.txt`. The upstream MIT license is retained in `LICENSE`.

Source: https://github.com/sigsep/open-unmix-pytorch/tree/v1.3.0

Copyright (c) 2019 Inria (Fabian-Robert Stöter and Antoine Liutkus).

The selected trained model is **UMX-HQ**, Zenodo DOI 10.5281/zenodo.3370489. The official record metadata explicitly declares `mit-license`; see `UMXHQ-UPSTREAM-METADATA.json`. Four original checkpoints are downloaded by setup, not included in this repository. SHA256 pins, original MD5 checksums, exact byte lengths and original URLs are in `tools/stems-models.json`.

This is not UMX-L. No UMX-L weights are downloaded. The worker does not contact a service while processing audio. It performs actual local neural inference and preserves the source audio.

The optional private macOS Python bootstrap downloads original CPython 3.11.11 distributions built by python-build-standalone release 20241206. The build project's source is MPL-2.0 (https://github.com/astral-sh/python-build-standalone/blob/20241206/LICENSE); CPython and bundled dependencies retain their own license notices in the downloaded runtime. The archives are not redistributed in this repository. Binary distribution metadata and source remain available from https://github.com/astral-sh/python-build-standalone/releases/tag/20241206 .

Python dependencies retain their respective package license notices. No claim of authorship, exclusive rights in trained weights, endorsement by the upstream authors, or equivalence to iZotope RX is made.
