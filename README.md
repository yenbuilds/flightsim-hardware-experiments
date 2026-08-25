# YenBuilds Flight Simulation Workshop

A working collection of flight-simulator hardware designs, MobiFlight configurations, autothrottle experiments, and reference imagery.

## Repository Map

| Folder | Contents |
| --- | --- |
| [`archive/`](archive/) | Deprecated autothrottle experiments, prototypes, and test configurations |
| [`configs/`](configs/) | MobiFlight exports grouped by device and aircraft |
| [`hardware/`](hardware/) | Printable flight-control models and enclosure designs |
| [`software/`](software/) | Supporting reference imagery |

## Current MobiFlight Configurations

- [`pmdg-737-current.mcc`](configs/mobiflight/pmdg-737/pmdg-737-current.mcc) is the most complete 737 export, including spoiler controls.
- [`pmdg-777-current.mcc`](configs/mobiflight/pmdg-777/pmdg-777-current.mcc) is the integrated autothrottle, taxi-light, and runway-light export.
- Earlier working exports are preserved in each aircraft's `snapshots/` folder.
- Review and rebind MobiFlight hardware inputs after import; exports retain original module-specific mappings.

## Working Conventions

- Use descriptive lowercase names; lead with aircraft or component, then purpose, then version or date.
- Keep source files and exported files together when an application relies on relative paths.
- Preserve working revisions until a replacement has been tested; keep earlier MobiFlight exports in the relevant `snapshots/` folder and move unrelated abandoned prototypes to `archive/`.
- Mark generated reference images with `-gpt-gen`.

## Licence

Released under the [MIT License](LICENSE).
