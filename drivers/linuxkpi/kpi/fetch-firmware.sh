#!/bin/bash
# Fetch the Linux firmware the LinuxKPI drivers ask for, from a pinned linux-firmware release, and verify it.
#   usage: fetch-firmware.sh <outdir>      then: cmake -DLKPI_FIRMWARE_DIR=<outdir> ...
# Files land in <outdir>/amdgpu/..., <outdir>/rtl_nic/... (request_firmware() names), copied by the build to
# reactos\system32\firmware on the CD. Licences: LICENSE.amdgpu (redistributable) and Realtek's notice from WHENCE,
# which must accompany the rtl_nic files.
#   amdgpu: Navi 31 (Radeon RX 7900 XT/XTX: GC 11.0.0, PSP/SMU 13.0.0, SDMA 6.0.0, VCN 4.0.0, DCN 3.2.0 DMCUB for the
#           display core)
#   rtl_nic: every patch r8169 may load (RTL8111/8168/8125/8126)
set -euo pipefail
TAG=20241110
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"
cd "$OUT"
while read -r sum file; do
  [ -n "$file" ] || continue
  if [ "$file" = "rtl_nic/LICENCE.rtl_nic.txt" ]; then
    mkdir -p rtl_nic
    curl -fsS --retry 3 "$URL/WHENCE?h=$TAG" | awk '/^Driver: r8169/,/^------/' | awk '/^Licence:/,/^------/' |
      grep -v "^------" | sed 's/^Licence://' > "$file"
  elif [ ! -f "$file" ] || ! echo "$sum  $file" | sha256sum -c --quiet - 2>/dev/null; then
    mkdir -p "$(dirname "$file")"
    curl -fsS --retry 3 -o "$file" "$URL/$file?h=$TAG"
  fi
  echo "$sum  $file" | sha256sum -c --quiet -
done <<'SUMS'
572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee  LICENSE.amdgpu
4307fd096e57c4aa4744d97d36f0de71c3439724f1174c71131448bd6dd04d9b  amdgpu/dcn_3_2_0_dmcub.bin
b4f8fc056b45709a6abf48e7885fb1b4ab8d3cc092cbfa2c554a78564a6403bc  amdgpu/gc_11_0_0_imu.bin
7ff26c66610e116d6cd874502244e82dadcfc20849973bf142601585c969e6c6  amdgpu/gc_11_0_0_me.bin
4e309076b9caa7f7c0daec15b4c3bdad346ab514ff09a9b17770c6733ea491ae  amdgpu/gc_11_0_0_mec.bin
3b30fd16d1a0df5006afab78d1117ad6b3b2bdc9c01712070ece20648d362dec  amdgpu/gc_11_0_0_mes.bin
50c7d4d9cf5fd98846e1fe5369aed872c0d637aa675019271481fb60b7173f6a  amdgpu/gc_11_0_0_mes1.bin
9d510df5a267717a64cd050deed0d0c6502597ac0b6153268ce38a888ab3971a  amdgpu/gc_11_0_0_mes_2.bin
ac49b031bdea9e90b219b874aa1592f48f0b84ae2164d2bffc8b74d5bd9706a7  amdgpu/gc_11_0_0_pfp.bin
dabd49039772d02f5fd5e48dc21d35ad52a6b1283b470dabca86ca159c4c7c8e  amdgpu/gc_11_0_0_rlc.bin
c667e7016261848ce3240f15196ae0f031428ca47b2d759e534653115c235d99  amdgpu/gc_11_0_0_rlc_1.bin
2a211d631750bda24c7c56cef3dbfc34bdae22e78d5d5e6e87ba421596af5609  amdgpu/psp_13_0_0_sos.bin
a8aa1123556c44b4a13fd826b6869f4f7fdc722585d3a5f6195c0d9c5af19c15  amdgpu/psp_13_0_0_ta.bin
1f363527ae9eb710dab0749bfd313aa2f993e06f46b1653190e5af4ec51f9c5e  amdgpu/sdma_6_0_0.bin
b9502af58995bacb3c5dd607be0f79aecf79766b580a82764c4c0b7de99fd356  amdgpu/smu_13_0_0.bin
1fca9e78419a0c547be612536379a921d35bb46975e523fc7c79f81775acfcd9  amdgpu/vcn_4_0_0.bin
41a9f157391058710a7dd405106b15ab6212340eaa5a7481c8d97b5c9ca4c8a0  rtl_nic/LICENCE.rtl_nic.txt
03f7ee92f1a7c1723ac7a7447ae006930c9bb07e1d77c3d12bc70cc0d855bb00  rtl_nic/rtl8105e-1.fw
b249074519d87ff8bde17185894333c993f25f747f221305cf69a5d7b4519674  rtl_nic/rtl8106e-1.fw
ba4f7ed4bac4768810ba8a6c118457f5912f61a0380aa4fb322093caf52de0ef  rtl_nic/rtl8106e-2.fw
edc3a14a76852c411c4af5514dae5d55be37873f56a7563f80db7a76f2dc1ba7  rtl_nic/rtl8107e-2.fw
3f6c328af0ceff5969c729edf0f535e8bb2a9b9e05d15b6be3199a3e6d421b99  rtl_nic/rtl8125a-3.fw
529bf1c25c97ff52b401090d00ff89cc22351012336e5a0c9662728a3ee909ef  rtl_nic/rtl8125b-2.fw
2ffa201aa7dec5e3726e268a10342b175ae8de4fa9f39f535bbbbf5cb8245879  rtl_nic/rtl8126a-2.fw
18ffaf3b873d217bf780503a80f04fe2b03c088ba2d0c2a3d626145cb8e53b26  rtl_nic/rtl8126a-3.fw
ded779f55b3fc5bb8576b2d2b6c724581c72160b3c2f88e33b42dc6c2c4e70b0  rtl_nic/rtl8168d-1.fw
35dd68c01a7b24f6e554bd2c0b9cf57cf962a826651f66e75ffcea37cf2c3bb9  rtl_nic/rtl8168d-2.fw
c38d6818a99336c8e43883bbc136f9fab3b1b470ec0e86c61bf5dac617fe94bd  rtl_nic/rtl8168e-1.fw
0a489929b270df1533645a75d55ff357fd66e42057aa4e198ab738930de39386  rtl_nic/rtl8168e-2.fw
1287fdd82ff2f60d75c9970c108b49cfd11456ad3fe29c9666b8539a35777dbf  rtl_nic/rtl8168e-3.fw
ed3794c651d38b807168c7c49431bb8036922b16766588c520a20287f277615d  rtl_nic/rtl8168f-1.fw
5586b2093502f7dec762f23091fa90d6d8731e7d873509072ce2eae23ab8ee99  rtl_nic/rtl8168f-2.fw
c526c06aa0ab8d9b57ba2b730bb1e40a1c3513f8591e238dceb2e0f45210ed27  rtl_nic/rtl8168fp-3.fw
f878ec719f78e0b104bc73362a9862611858bec893f58a1b3b289a0a57d71d25  rtl_nic/rtl8168g-2.fw
e4d5e62b0a5a097af34c7aa3c55f634fd5be2c232e02c6b983ebd7e699cb27e9  rtl_nic/rtl8168g-3.fw
0b4beab008e308f28296c13188ee23edfeff7d865e398a7ade170495c8cec7e6  rtl_nic/rtl8168h-2.fw
a14adee93b7c7ecc738137d910334761ecf9c64d3addeccbd024a2f902f7f808  rtl_nic/rtl8402-1.fw
e66a3183787c80987e1e36c9f7d4b2c3cd5d976640b83f462702f914febc4b38  rtl_nic/rtl8411-1.fw
80dbdacba4d5dfee46b6c2b4e433f6f2eeb756d29ad46bf2362ba20c8ec594b1  rtl_nic/rtl8411-2.fw
SUMS
echo "firmware ready in $OUT ($(find "$OUT" -type f | wc -l) files)"
