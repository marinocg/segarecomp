# Master System gameplay example, native C11 form

The same rules and the same presentation as [`../gameplay`](../gameplay), written against `segarecomp_mod_sms.h` (SEG-147-T011, ADR 0325). `tests/mod_sms_lifecycle_test.py` requires the two programs to
produce identical guest state, write witness and presented pictures. Build the cartridge with `../gameplay/fixture_rom.py`.

A native provider is trusted code compiled into the program: it is **not** sandboxed, and installing it asks for approval of the exact package identity.

```sh
segarecomp mod build . --out /tmp/runner-native.segapatch --cc "$CC" --sdk-dir "$SDK"
segarecomp build --rom /tmp/runner.sms --output /tmp/out --cc "$CC" --runtime-dir "$SDK/master-system" --platform master-system \
  --native-package /tmp/runner-native.segapatch --native-identity <package_identity>
```
