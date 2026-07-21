# Generated assets

Produced by `tools/meshy/meshy_client.py`. One directory per asset, named
`<slug>_<hash>` where the hash covers the prompt and every parameter that
changes the result - so an unchanged request is a cache hit and spends nothing.

`manifest.json` is tracked; the GLB and texture binaries are not. The repository
is public and git history is permanent, so committing many-MB generated binaries
is a decision that cannot be undone. The manifest records the prompt, parameters,
task ids, and credits consumed, which is what makes a result reproducible and
reviewable. Selected production-ready cooked outputs ship through Git LFS under
`assets/runtime/`; raw provider downloads remain outside Git history.

Regenerating from a manifest costs credits and is not bit-identical: these models
are sampled, not deterministic.

Plan a current Meshy 6 preview without credentials or spend:

```powershell
python tools/meshy/meshy_client.py generate `
  --prompt "modular spaceship pressure door" `
  --name pressure_door `
  --asset-id ship.reference.fighter `
  --module-id airlock_module `
  --preview-only --dry-run
```

Remove `--dry-run` only after reviewing the request and cost. Keep
`--preview-only` until geometry is approved, then rerun the exact command without
it to refine the cached preview. Generated art is not production-eligible until
its assembly manifest passes `tools/validate_asset_manifest.py` and DCC cleanup,
collision, navigation, pivots, LODs, and runtime acceptance are complete.
