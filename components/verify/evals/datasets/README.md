# Benchmark cases from public datasets

The Tier 2 benchmark's first 78 cases came from one industrial site. These scripts add cases from public
datasets whose images can be republished on the public method page, so the ranking covers other sites, night,
animals and the objects that fool detectors.

| Source | Licence | What it adds | Truth comes from |
|---|---|---|---|
| [MEVA](https://mevadata.org/) (Kitware / IARPA DIVA) | CC BY 4.0 | another site: 20+ fixed surveillance cameras, indoor and outdoor, people and vehicles | actor and vehicle boxes (activity actors only, so MEVA gives real cases only) |
| [ENA24-detection](https://lila.science/datasets/ena24detection) (LILA BC) | CDLA-Permissive-1.0 | trail cameras, day and infrared night, 23 animal classes; images with people were removed | every animal boxed, so any other detection is false |
| [Open Images V7](https://storage.googleapis.com/openimages/web/index.html) validation | CC BY 2.0 per image | snow, fog, rain, dark streets, parking lots; statues, dolls, toys, posters | boxes (with IsDepiction and IsGroupOf) and human-verified negative labels |

Considered and left out: VIRAT (no redistribution), LLVIP (non-commercial, infrared images restricted),
DAWN (all rights reserved), COCO (per-image Flickr licences include non-commercial), ExDark (commercial use
needs the authors' permission).

## Build

```sh
cd components/verify
D=/srv/data/cvkit/verify/datasets
uv run python -m evals.datasets.openimages $D/openimages/raw $D/openimages        # CSVs from storage.googleapis.com/openimages
uv run --with pyarrow python -m evals.datasets.ena24 $D/ena24/raw $D/ena24        # parquet shards from HF lila-bc-community/ena24-detection
uv run python -m evals.datasets.meva <meva-data-repo checkout> $D/meva             # gitlab.kitware.com/meva/meva-data-repo
uv run python -m evals.datasets.build seq $D/seq $D/openimages $D/ena24 $D/meva

# On the edge Pi (stop cvkit-detect first: it holds the Hailo exclusively)
rsync -a $D/seq/ pi@edge:/tmp/seq/ && scp evals/datasets/pi_detect.py pi@edge:/tmp/
ssh pi@edge 'sudo systemctl stop cvkit-detect; /opt/cvkit/venv/bin/python /tmp/pi_detect.py /tmp/seq /tmp/dets.json; sudo systemctl start cvkit-detect'
scp pi@edge:/tmp/dets.json $D/

uv run python -m evals.datasets.build cases /srv/data/cvkit/verify/cases-v2 $D/dets.json $D/seq \
    --take meva=80 ena24=70 openimages=80
```

`pi_detect.py` runs the live model (`yolov8s_h8l.hef`, NMS threshold 0.3, `libyolo_hailortpp_post.so`) on
images letterboxed exactly as cvkit-detect letterboxes camera frames, so every claim is real detector output.
`build.py cases` keeps a detection only when its truth is certain (see its docstring) and draws the snapshot
the way Tier 1 does. Each case records its source, scene tags and image credit for the method page.
