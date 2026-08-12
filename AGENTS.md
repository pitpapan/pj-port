## Zephyr Source Tree

Do not inspect, search, index, or modify files under `zephyr/`.

Treat Zephyr as an external platform dependency.

Use documented Zephyr interfaces and build commands instead of studying
the Zephyr source tree.

Allowed operations include:

- `west --version`
- `west topdir`
- `west list`
- `west boards`
- `west build`
- `west build -t run`
- reading compiler/build output
- working with files under our own application and PJPROJECT port

Do not run recursive searches such as:

- `grep -R ... zephyr/`
- `rg ... zephyr/`
- `find zephyr/ ...`

If resolving a problem genuinely requires inspecting Zephyr implementation
source, stop and report why it is necessary instead of reading it automatically.

Reading any file under zephyr/ is prohibited unless I explicitly authorize that specific inspection.