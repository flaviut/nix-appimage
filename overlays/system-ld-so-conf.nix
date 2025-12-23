final: prev:
let
  appendSystemLdConf = drv:
    drv.overrideAttrs (old: {
      postFixup = (old.postFixup or "") + ''
        target="$out/etc/ld.so.conf"
        mkdir -p "$(dirname "$target")"
        if [ -e "$target" ] && grep -qE '^include /etc/ld\.so\.conf$' "$target"; then
          :
        else
          echo "include /etc/ld.so.conf" >> "$target"
        fi
      '';
    });
in
{
  glibc = appendSystemLdConf prev.glibc;
} // (if prev ? glibc_multi then { glibc_multi = appendSystemLdConf prev.glibc_multi; } else { })
