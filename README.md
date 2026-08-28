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

- [`pmdg-737-current.public.mcc`](configs/mobiflight/pmdg-737/pmdg-737-current.public.mcc) is the most complete 737 export, including spoiler controls.
- [`pmdg-777-current.public.mcc`](configs/mobiflight/pmdg-777/pmdg-777-current.public.mcc) is the integrated autothrottle, taxi-light, and runway-light export.
- Earlier working exports are preserved in each aircraft's `snapshots/` folder.
- Review and rebind MobiFlight hardware inputs after import; public exports preserve the mappings but replace physical controller serials with stable placeholders.

### Private and Public Bindings

Raw `*.mcc` exports retain exact physical controller serials, so they are ignored by Git and remain fully bound in the owner's local checkout. Shareable `*.public.mcc` siblings use three distinct dummy serials (`SN-PUB-ATC`, `SN-PUB-MCP`, and `SN-PUB-LGT`) so the controller roles do not collapse into one missing device.

Changing a serial necessarily removes its exact hardware binding. MobiFlight may automatically match another controller of the same type; otherwise use **Extras > Controller Bindings** as described in the [official binding guide](https://docs.mobiflight.com/features/controller-bindings/).

Regenerate public copies from the ignored, locally bound exports:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/sync-mobiflight-configs.ps1 -Mode Publish
```

The publish step also saves the real-to-public mapping under the ignored `.local/` directory. To reconstruct fully bound copies under `.local/materialized/`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/sync-mobiflight-configs.ps1 -Mode Materialize
```

## Working Conventions

- Use descriptive lowercase names; lead with aircraft or component, then purpose, then version or date.
- Keep source files and exported files together when an application relies on relative paths.
- Preserve working revisions until a replacement has been tested; keep earlier MobiFlight exports in the relevant `snapshots/` folder and move unrelated abandoned prototypes to `archive/`.
- Mark generated reference images with `-gpt-gen`.

## Licence

Released under the [MIT License](LICENSE).
