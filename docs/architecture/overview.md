# Initial architecture

The first build has three small layers:

- `memory::GuestMemory` owns a bounded byte buffer and exposes big-endian 8-, 16-, 32- and 64-bit accesses. Multi-byte accesses enforce natural alignment in this prototype. It is a test memory model, not the Xbox 360 virtual memory system.
- `cpu::decode` converts a 32-bit PowerPC instruction word into a typed operation or an explicit diagnostic.
- `cpu::Interpreter` executes one instruction or a bounded sequence against `CpuState` and guest memory. Current compare instructions update the selected CR field; conditional branches use CR bits and CTR according to their BO/BI fields. Unsupported or reserved forms return a fault instead of silently succeeding.

`CpuState` includes 32 FPR bit containers. The implemented scalar transfer subset is `lfs`, `lfd`, `stfs`, and `stfd` in D and indexed forms, including update variants. `lfs` widens an IEEE binary32 value into the binary64 FPR representation; `stfs` converts back to binary32. Floating arithmetic, FPSCR status/rounding control, denormal/NaN exception behavior and VMX128 are not implemented, so these transfers alone do not support floating-point programs generally. Opcode assignments were checked against IBM's [PowerPC instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=reference-appendix-f-powerpc-instructions).

## Memory mapping prototype

`GuestMemory` retains a bounded byte buffer as physical backing storage. Page mapping is opt-in, uses a fixed 4 KiB page size in this prototype, supports aliases, and checks read/write/execute access separately. CPU instruction fetch now requests execute permission, while data operations request read or write permission. `load()` is a host-side initialization path to populate backing bytes. This is generic virtual-to-physical translation only: it has no Xenon segment/page-table rules, MMIO, shared CPU/GPU coherence, concurrent access synchronization, demand paging, or 512 MB console memory model.

## XEX2 metadata parser

`loader::parse_xex2` validates the fixed XEX2 header, optional-header directory, security-info bounds, page-descriptor count and file-format information. `parse_xex2_file` reads only the declared bounded header, while `load_xex2_file` applies a 512 MiB file-size cap. The loader supports only encryption type 0 and compression type 0. It validates the Xbox PowerPC big-endian PE32 machine/subsystem, image bounds, section ranges, header overlap and section overlap, then transactionally maps pages and copies PE headers/sections into guest memory with read/write/execute permissions. A synthetic test image reaches the interpreter and runs one instruction. Encrypted/compressed payloads, imports, relocation and kernel initialization remain unsupported; this is not yet commercial game startup. The format fields and machine/subsystem checks are cross-checked against the public [Xenia XEX definitions](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/util/xex2_info.h) and [PE validation flow](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/xex_module.cc).

`project360_core` contains reusable emulator code, `project360` is the smoke-program executable, and `project360_tests` is a standalone CTest target. Frontends, guest loaders and platform graphics/audio APIs are intentionally not dependencies of the CPU core.

## Program exceptions and fault reporting

`StepResult` attaches an `ExecutionFault` to interpreter faults. The record classifies illegal and privileged instructions, instruction alignment/access faults, data alignment/access faults, and internal consistency errors; it includes the faulting PC, fetched word when available, and effective address when applicable. Illegal and privileged instructions enter the PowerPC program exception vector at `0x700`, save SRR0/SRR1, and report a `GuestException`. The current implementation handles the illegal-instruction and privileged-instruction syndrome bits, and `rfid` restores a subset of MSR state and resumes at SRR0. SRR0/SRR1 `mfspr`/`mtspr` transfers are restricted when modeled MSR.PR is set.

This is a narrow PowerPC exception subset, not the Xenon exception system: data/instruction access and alignment faults remain host diagnostics, and other exception classes, full MSR behavior, hypervisor/LPES configuration and interrupt delivery are not implemented. Exception save/restore follows the public [PowerPC Operating Environment Architecture, Book III v2.02](https://powerpc.dev/general/PPC_Vers202_Book3_public.pdf) for this limited case; `rfid` encoding and behavior are cross-checked against IBM's [instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-rfid-return-from-interrupt-double-word-instruction).

## Integer compare and branch references

The `cmp`/`cmpl` and `cmpi`/`cmpli` implementations follow the architecture's signed/unsigned comparison, L-width, selected-CR-field, and XER summary-overflow behavior. `addc`/`adde` and `addic` forms update XER.CA; `subfc`/`subfe`/`subfic` set CA to the subtraction carry-out (1 means no borrow), while ordinary `subf` leaves CA unchanged. OE forms update XER.OV and sticky XER.SO. Immediate `b`/`bc` instructions support relative and absolute targets; `bclr`/`bcctr` apply the decoded BO condition and use aligned LR/CTR targets. Unsupported BO/BH encodings and invalid `bcctr` counter-decrement forms fault explicitly.

The register logical operations use full 64-bit GPR values and update CR0 only in record forms. `slw`/`srw` operate on the low word and zero-extend their result; `sraw` sign-extends the low-word result and updates XER.CA when a negative value loses one bits. `rlwinm` rotates the low word, uses PowerPC's MSB-numbered inclusive mask endpoints and zero-extends the masked word.

`srawi` uses the instruction's five-bit immediate shift count, sign-extends the shifted low word to the 64-bit GPR, and sets XER.CA when a negative source shifts out any one bits. Rc updates CR0, and a zero shift clears CA.

The PPC64 `sld`/`srd` operations use RB's low seven bits as the shift count and return zero for counts 64–127. `srad` uses arithmetic sign fill; XER.CA is set for negative sources when any one bits are shifted out, and for counts 64–127 it follows the sign bit. Rc updates CR0. Unsupported shift instructions remain explicit decode faults.

`cntlzw` counts leading zero bits in the source register's low word, while `cntlzd` counts across the full doubleword. Both return 32 or 64 respectively for a zero source, may update CR0 when Rc is set, and leave XER unchanged. Their reserved RB field is checked by the decoder.

The immediate logical forms use unsigned 16-bit constants: `ori`/`xori`/`andi.` use the low half, while their shifted forms place the immediate in bits 16–31. `andi.` and `andis.` always update CR0 and leave XER unchanged; `ori`, `oris`, `xori`, and `xoris` leave condition and exception state unchanged. The initial multiply subset implements PPC64 `mulli` low-doubleword multiplication, signed `mullw` from low-word operands with its full signed product in the 64-bit GPR, and signed `mulld` low-doubleword multiplication. `mullw`/`mulld` support Rc and OE; overflow updates OV and sticky SO, and Rc records the result in CR0. The sign-extension instructions `extsb`, `extsh`, and PPC64-only `extsw` sign-extend the low byte, halfword, or word to the GPR width; Rc updates CR0 and these forms leave XER unchanged.

The data subset includes D-form accesses plus indexed `lbzx`, `lhzx`, `lhax`, `lwzx`, `ldx`, `stbx`, `sthx`, `stwx`, and `stdx`; indexed update forms `lbzux`, `lhzux`, `lhaux`, `lwzux`, `ldux`, `stbux`, `sthux`, `stwux`, and `stdux`; byte-reversed `lhbrx`, `lwbrx`, `sthbrx`, and `stwbrx`; D-form `lmw`/`stmw`; and DS-form `ld`/`ldu`/`std`/`stdu`. DS displacement is a signed 14-bit quantity scaled by four; the unsupported `lwa` DS form is diagnosed. Indexed effective addresses add GPR RA and RB, treating RA=0 as a zero base for non-update forms. Update forms reject RA=0; update loads also reject RT=RA. `lmw`/`stmw` transfer GPR RT through GPR 31 as aligned, big-endian words. `lmw` rejects base/destination overlap forms whose result is architecturally undefined. For a failing multiword access, this flat-memory prototype prevalidates the entire range; `lmw` stages loaded values before changing registers. Byte accesses may use any address; wider accesses require natural alignment in this prototype. Byte-reversed forms reverse bytes within a naturally aligned halfword or word. Loads zero-extend except for `lha`/`lhau`/`lhax`/`lhaux`, which sign-extend to the 64-bit GPR. D-form and indexed update loads/stores change RA only after successful access. Guest address mapping, permissions, MMIO and the Xbox 360 memory model remain outside this flat buffer.

- [IBM `add` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-add-add-cax-compute-address-instruction)
- [IBM `addc` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-addc-add-carrying-instruction)
- [IBM `adde` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-adde-ae-add-extended-instruction)
- [IBM `addic` instruction reference](https://www.ibm.com/docs/sv/ssw_aix_72/assembler/idalangref_addic_inst.html)
- [IBM `addic.` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-addic-ai-add-immediate-carrying-record-instruction)
- [IBM `subf` instruction reference](https://www.ibm.com/docs/fi/aix/7.2.0?topic=set-subf-subtract-from-instruction)
- [IBM `subfc` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-subfc-sf-subtract-from-carrying-instruction)
- [IBM `subfe` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-subfe-sfe-subtract-from-extended-instruction)
- [IBM `subfic` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-subfic-sfi-subtract-from-immediate-carrying-instruction)
- [IBM `and` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-instruction)
- [IBM `slw` instruction reference](https://www.ibm.com/docs/sv/ssw_aix_72/assembler/idalangref_slw_slword_instrs.html)
- [IBM `srw` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-srw-sr-shift-right-word-instruction)
- [IBM `sraw` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-sraw-sra-shift-right-algebraic-word-instruction)
- [IBM `rlwinm` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=is-rlwinm-rlinm-rotate-left-word-immediate-then-mask-instruction)
- [IBM `xori` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-xori-xoril-xor-immediate-instruction)
- [IBM `xoris` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-xoris-xoriu-xor-immediate-shift-instruction)
- [IBM `andi.` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-andi-andil-immediate-instruction)
- [IBM `andis.` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-andis-andiu-immediate-shifted-instruction)
- [IBM `lbz` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-lbz-load-byte-zero-instruction)
- [IBM `lhz` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-lhz-load-half-zero-instruction)
- [IBM `lha` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-lha-load-half-algebraic-instruction)
- [IBM `stb` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-stb-store-byte-instruction)
- [IBM `sth` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-sth-store-half-instruction)
- [IBM `lhau` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-lhau-load-half-algebraic-update-instruction)
- [IBM fixed-point load/store update rules](https://www.ibm.com/docs/en/aix/7.3.0?topic=processor-fixed-point-load-store-update-instructions)
- [IBM PowerPC instruction opcode table](https://www.ibm.com/docs/en/aix/7.3.0?topic=reference-appendix-b-instruction-set-sorted-by-mnemonic)
- [IBM `lhzx` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-lhzx-load-half-zero-indexed-instruction)
- [IBM `lwzx` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-lwzx-lx-load-word-zero-indexed-instruction)
- [IBM `stwx` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-stwx-stx-store-word-indexed-instruction)
- [IBM `lbzux` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-lbzux-load-byte-zero-update-indexed-instruction)
- [IBM `lhzux` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-lhzux-load-half-zero-update-indexed-instruction)
- [IBM `stbux` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-stbux-store-byte-update-indexed-instruction)
- [IBM `lwzux` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=is-lwzux-lux-load-word-zero-update-indexed-instruction)
- [IBM `stwux` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-stwux-stux-store-word-update-indexed-instruction)
- [IBM `lwbrx` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=is-lwbrx-lbrx-load-word-byte-reverse-indexed-instruction)
- [IBM `stwbrx` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=is-stwbrx-stbrx-store-word-byte-reverse-indexed-instruction)
- [IBM `sthbrx` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-sthbrx-store-half-byte-reverse-indexed-instruction)
- [IBM PowerPC instruction appendix (`lhbrx`, XO 790)](https://www.ibm.com/docs/en/aix/7.1.0?topic=reference-appendix-f-powerpc-instructions)
- [IBM `lmw` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-lmw-lm-load-multiple-word-instruction)
- [IBM `stmw` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-stmw-stm-store-multiple-word-instruction)
- [PowerPC User Instruction Set Architecture, Version 2.02 (`mulli`, `mullw`, `mulld`)](https://powerpc.dev/general/PPC_Vers202_Book1_public.pdf)
- [IBM `mullw` instruction reference](https://www.ibm.com/docs/en/aix/7.1?topic=set-mullw-muls-multiply-low-word-instruction)
- [IBM `mulld` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-mulld-multiply-low-double-word-instruction)
- [IBM `extsb` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-extsb-extend-sign-byte-instruction)
- [IBM `extsh` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-extsh-exts-extend-sign-halfword-instruction)
- [PowerPC User Instruction Set Architecture, Version 2.02 (`extsw` semantics)](https://powerpc.dev/general/PPC_Vers202_Book1_public.pdf)
- [IBM `cmp` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-cmp-compare-instruction)
- [IBM `cmpl` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-cmpl-compare-logical-instruction)
- [IBM `cmpi` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-cmpi-compare-immediate-instruction)
- [IBM `cmpli` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-cmpli-compare-logical-immediate-instruction)
- [IBM `bc` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-bc-branch-conditional-instruction)
- [IBM `b` instruction reference](https://www.ibm.com/docs/nl/aix/7.2.0?topic=set-b-branch-instruction)
- [IBM `bclr` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-bclr-bcr-branch-conditional-link-register-instruction)
- [IBM `bcctr` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-bcctr-bcc-branch-conditional-count-register-instruction)
- [IBM `ld` instruction reference](https://www.ibm.com/docs/tr/aix/7.3?topic=set-ld-load-doubleword-instruction)
- [IBM `ldu` instruction reference](https://www.ibm.com/docs/pt-br/aix/7.3?topic=set-ldu-load-doubleword-update-instruction)
- [IBM `std` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-std-store-double-word-instruction)
- [IBM `stdu` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-stdu-store-double-word-update-instruction)
- [IBM PowerPC instruction appendix (`ldx`, `ldux`, `stdx`, `stdux`)](https://www.ibm.com/docs/it/aix/7.1.0?topic=reference-appendix-f-powerpc-instructions)
- [IBM `sld` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-sld-shift-left-double-word-instruction)
- [IBM `srd` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-srd-shift-right-double-word-instruction)
- [IBM `srad` instruction reference](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-srad-shift-right-algebraic-double-word-instruction)
- [IBM `cntlzw` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-cntlzw-cntlz-count-leading-zeros-word-instruction)
- [IBM `cntlzd` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-cntlzd-count-leading-zeros-double-word-instruction)
- [IBM `srawi` instruction reference](https://www.ibm.com/docs/da/ssw_aix_71/assembler/idalangref_srawi_srai_instrs.html)
- [IBM PowerPC instruction fields](https://www.ibm.com/docs/en/aix/7.3.0?topic=forms-instruction-fields)
