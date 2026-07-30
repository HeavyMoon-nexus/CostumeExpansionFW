{
  CEF_MergeAudit.pas -- READ-ONLY plugin audit for CostumeExpansionFW support.

  What it does
  ------------
  Dumps, for every ARMO (armor) record in the plugins you selected:
    - FormID (load-order + local), EditorID, display name
    - biped slot mask (BOD2)
    - attached scripts (VMAD), enchantment link (EITM)
    - every linked ARMA (armor addon): race + additional races, slot mask,
      all four model paths, and whether each model file actually exists
      (loose file or BSA)
    - unresolved/missing ARMA links
  plus a per-file header (ESL flag, masters) and a summary of everything
  suspicious it found.

  It NEVER modifies anything. It only reads and writes a text report to
  <xEdit folder>\CEF_MergeAudit_output.txt. You can open this file in any
  text editor to verify what it does before running it.

  How to run (SSEEdit 4.x)
  ------------------------
  1. Copy this file into SSEEdit's "Edit Scripts" folder.
  2. Start SSEEdit, load your usual load order.
  3. In the left tree, select ONLY the plugin(s) to audit
     (e.g. "Clothing Loot2.esp" -- ctrl-click to add more).
  4. Right-click the selection -> "Apply Script..." -> pick "CEF_MergeAudit".
  5. When it finishes, send the file CEF_MergeAudit_output.txt
     (it is created next to SSEEdit.exe; the exact path is also printed
     at the end of the run).
}
unit CEFMergeAudit;

var
  slOut: TStringList;       // the report
  slFiles: TStringList;     // file names already dumped (header printed once)
  totalArmo: integer;
  totalArma: integer;
  cntMissingArmaLink: integer;   // ARMO -> ARMA link that resolves to nothing
  cntNoArmature: integer;        // ARMO with an empty armature list
  cntMissingModel: integer;      // model path that does not exist on disk/BSA
  cntNoModelPath: integer;       // ARMA with no model path at all
  cntVmad: integer;              // ARMO with attached scripts
  cntTargetHits: integer;        // records matching the crash-log FormIDs

procedure Emit(const s: string);
begin
  slOut.Add(s);
  AddMessage(s);
end;

// 'meshes\' prefix + backslashes, as ResourceExists expects a Data-relative path.
function NormalizeModelPath(p: string): string;
begin
  Result := StringReplace(p, '/', '\', [rfReplaceAll]);
  if (Result <> '') and (Pos('meshes\', LowerCase(Result)) <> 1) then
    Result := 'meshes\' + Result;
end;

// One candidate element: if it is a MOD2..MOD5 model-path leaf, print the
// path + whether the file exists. Returns 1 when it was a model leaf.
function TryModelLeaf(leaf: IInterface): integer;
var
  sig, p, norm, label_: string;
begin
  Result := 0;
  sig := Copy(Name(leaf), 1, 4);
  if sig = 'MOD2' then label_ := 'MOD2 male world'
  else if sig = 'MOD3' then label_ := 'MOD3 female world'
  else if sig = 'MOD4' then label_ := 'MOD4 male 1st'
  else if sig = 'MOD5' then label_ := 'MOD5 female 1st'
  else Exit;
  Result := 1;
  p := GetEditValue(leaf);
  if p = '' then begin
    Emit('        ' + label_ + ': (empty)');
    Exit;
  end;
  norm := NormalizeModelPath(p);
  if ResourceExists(norm) then
    Emit('        ' + label_ + ': ' + p + '  [ok]')
  else begin
    Emit('        ' + label_ + ': ' + p + '  [<<< FILE NOT FOUND]');
    Inc(cntMissingModel);
  end;
end;

// Model paths are located by SUBRECORD NAME PREFIX (MOD2..MOD5), never by
// container display name: a display-name path ('Female world model\MOD3')
// silently missed every world model on SSEEdit 4.1.5 (262 false "no model"
// rows on a pack that renders fine in game; verified against the record's
// actual data). A two-level walk over the ARMA's children cannot miss them,
// whatever the containers happen to be called in a given xEdit build.
function DumpArmaModels(a: IInterface): integer;
var
  i, j: integer;
  c: IInterface;
begin
  Result := 0;
  for i := 0 to ElementCount(a) - 1 do begin
    c := ElementByIndex(a, i);
    Result := Result + TryModelLeaf(c);
    for j := 0 to ElementCount(c) - 1 do
      Result := Result + TryModelLeaf(ElementByIndex(c, j));
  end;
end;

// Per-file header, printed once per plugin encountered.
procedure DumpFileHeader(f: IInterface);
var
  fh, masters: IInterface;
  i: integer;
  flags: cardinal;
  eslText: string;
begin
  fh := ElementByIndex(f, 0);  // TES4 header
  flags := GetElementNativeValues(fh, 'Record Header\Record Flags');
  if (flags and $200) <> 0 then
    eslText := 'YES (light)'
  else
    eslText := 'no';
  Emit('');
  Emit('================================================================');
  Emit('FILE: ' + GetFileName(f));
  Emit('  ESL-flagged: ' + eslText);
  masters := ElementByPath(fh, 'Master Files');
  if Assigned(masters) then begin
    Emit('  Masters (' + IntToStr(ElementCount(masters)) + '):');
    for i := 0 to ElementCount(masters) - 1 do
      Emit('    - ' + GetElementEditValues(ElementByIndex(masters, i), 'MAST'));
  end else
    Emit('  Masters: none');
  Emit('================================================================');
end;

// Scripts attached to the record (VMAD).
procedure DumpVmad(e: IInterface);
var
  scripts: IInterface;
  i: integer;
  nm: string;
begin
  if not ElementExists(e, 'VMAD') then
    Exit;
  Inc(cntVmad);
  Emit('    VMAD: HAS ATTACHED SCRIPT(S)');
  scripts := ElementByPath(e, 'VMAD\Scripts');
  if Assigned(scripts) then
    for i := 0 to ElementCount(scripts) - 1 do begin
      nm := GetElementEditValues(ElementByIndex(scripts, i), 'scriptName');
      if nm = '' then
        nm := '(unnamed entry)';
      Emit('      script: ' + nm);
    end;
end;

// One ARMA reached from an ARMO.
procedure DumpArma(a: IInterface);
var
  addl: IInterface;
  i: integer;
  s: string;
begin
  Inc(totalArma);
  Emit('      ARMA ' + IntToHex(GetLoadOrderFormID(a), 8) + ' "' + EditorID(a) +
    '" (in ' + GetFileName(GetFile(a)) + ')');
  if ElementExists(a, 'BOD2') then
    Emit('        slots: ' +
      IntToHex(GetElementNativeValues(a, 'BOD2\First Person Flags'), 8))
  else
    Emit('        slots: (no BOD2)');
  s := GetElementEditValues(a, 'RNAM');
  if s = '' then
    s := '(none)';
  Emit('        race: ' + s);
  addl := ElementByPath(a, 'Additional Races');
  if Assigned(addl) and (ElementCount(addl) > 0) then begin
    Emit('        additional races (' + IntToStr(ElementCount(addl)) + '):');
    for i := 0 to ElementCount(addl) - 1 do
      Emit('          - ' + GetEditValue(ElementByIndex(addl, i)));
  end;
  if DumpArmaModels(a) = 0 then begin
    Emit('        model: <<< NO MODEL PATH ON ANY SLOT');
    Inc(cntNoModelPath);
  end;
end;

function Initialize: integer;
begin
  Result := 0;
  slOut := TStringList.Create;
  slFiles := TStringList.Create;
  totalArmo := 0;
  totalArma := 0;
  cntMissingArmaLink := 0;
  cntNoArmature := 0;
  cntMissingModel := 0;
  cntNoModelPath := 0;
  cntVmad := 0;
  cntTargetHits := 0;
  Emit('CEF_MergeAudit -- read-only ARMO/ARMA report');
  Emit('generated by CostumeExpansionFW support tooling');
end;

function Process(e: IInterface): integer;
var
  f, arm, link: IInterface;
  i: integer;
  localId: cardinal;
  fullName, eitm: string;
begin
  Result := 0;
  if Signature(e) <> 'ARMO' then
    Exit;

  f := GetFile(e);
  if slFiles.IndexOf(GetFileName(f)) < 0 then begin
    slFiles.Add(GetFileName(f));
    DumpFileHeader(f);
  end;

  Inc(totalArmo);
  localId := GetLoadOrderFormID(e) and $FFFFFF;
  fullName := GetElementEditValues(e, 'FULL');
  Emit('');
  Emit('  ARMO ' + IntToHex(GetLoadOrderFormID(e), 8) +
    '  local ' + IntToHex(localId, 6) +
    '  "' + EditorID(e) + '"  name="' + fullName + '"');

  // The two content ids seen in the reporter's crash data. A hit here is not
  // an accusation -- it just marks the records we most want to see.
  if (localId = $0017E9) or (localId = $000DDD) then begin
    Inc(cntTargetHits);
    Emit('    <<< THIS RECORD MATCHES A FORMID FROM THE CRASH LOGS');
  end;

  if ElementExists(e, 'BOD2') then
    Emit('    slots: ' +
      IntToHex(GetElementNativeValues(e, 'BOD2\First Person Flags'), 8))
  else
    Emit('    slots: (no BOD2 -- templated?)');

  eitm := GetElementEditValues(e, 'EITM');
  if eitm <> '' then
    Emit('    enchant: ' + eitm);

  DumpVmad(e);

  arm := ElementByPath(e, 'Armature');
  if (not Assigned(arm)) or (ElementCount(arm) = 0) then begin
    Emit('    armature: <<< EMPTY (no ARMA at all -- nothing can render)');
    Inc(cntNoArmature);
    Exit;
  end;
  Emit('    armature (' + IntToStr(ElementCount(arm)) + '):');
  for i := 0 to ElementCount(arm) - 1 do begin
    link := LinksTo(ElementByIndex(arm, i));
    if not Assigned(link) then begin
      Emit('      <<< UNRESOLVED ARMA LINK (entry ' + IntToStr(i) + ')');
      Inc(cntMissingArmaLink);
    end else
      DumpArma(link);
  end;
end;

function Finalize: integer;
var
  outPath: string;
begin
  Result := 0;
  Emit('');
  Emit('================================================================');
  Emit('SUMMARY');
  Emit('  files scanned:              ' + IntToStr(slFiles.Count));
  Emit('  ARMO records:               ' + IntToStr(totalArmo));
  Emit('  ARMA records reached:       ' + IntToStr(totalArma));
  Emit('  crash-log FormID matches:   ' + IntToStr(cntTargetHits));
  Emit('  -- problems ------------------------------------------------');
  Emit('  unresolved ARMA links:      ' + IntToStr(cntMissingArmaLink));
  Emit('  ARMO with empty armature:   ' + IntToStr(cntNoArmature));
  Emit('  model files not found:      ' + IntToStr(cntMissingModel));
  Emit('  ARMA with no model path:    ' + IntToStr(cntNoModelPath));
  Emit('  ARMO with scripts (VMAD):   ' + IntToStr(cntVmad));
  Emit('================================================================');

  outPath := ProgramPath + 'CEF_MergeAudit_output.txt';
  slOut.SaveToFile(outPath);
  AddMessage('');
  AddMessage('>>> report written to: ' + outPath);
  AddMessage('>>> please send that file back. nothing was modified.');

  slOut.Free;
  slFiles.Free;
end;

end.
