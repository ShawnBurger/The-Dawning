# Generated assets

Produced by `tools/meshy/meshy_client.py`. One directory per asset, named
`<slug>_<hash>` where the hash covers the prompt and every parameter that
changes the result - so an unchanged request is a cache hit and spends nothing.

`manifest.json` is tracked; the GLB and texture binaries are not. The repository
is public and git history is permanent, so committing many-MB generated binaries
is a decision that cannot be undone. The manifest records the prompt, parameters,
task ids, and credits consumed, which is what makes a result reproducible and
reviewable. Git LFS is the answer if binaries ever need to ship.

Regenerating from a manifest costs credits and is not bit-identical: these models
are sampled, not deterministic.
