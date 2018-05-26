type state = {
  rootPath: string,
  localCompiledBase: string,
  localModules: list((string, (string, string))),
  localCompiledMap: list((string, string)),
  includeDirectories: list(string),
  dependencyModules: list((FindFiles.modpath, (string, string))),
  cmtCache:
    Hashtbl.t(
      string,
      (
        float, /* modified time */
        Cmt_format.cmt_infos,
        (option(string), list(Docs.full))
      )
    ),
  pathsForModule: Hashtbl.t(string, (string, string)),
  documentText: Hashtbl.t(string, (string, int, bool)),
  compiledDocuments: Hashtbl.t(string, AsYouType.result),
  documentTimers: Hashtbl.t(string, float),
  compilationFlags: string,
  /* workspace folders... */
};

let isMl = path =>
  Filename.check_suffix(path, ".ml") || Filename.check_suffix(path, ".mli");

let odocToMd = text => {
  let top = MarkdownOfOCamldoc.convert(0, text);
  Omd.to_markdown(top);
};

let docConverter = src => isMl(src) ? odocToMd : (x => x);

let newDocs = (cmtCache, changed, cmt, src) => {
  let infos = Cmt_format.read_cmt(cmt);
  switch (Docs.forCmt(docConverter(src), infos)) {
  | None => None
  | Some(docs) =>
    Hashtbl.replace(cmtCache, cmt, (changed, infos, docs));
    Some(docs);
  };
};

let hasProcessedCmt = (state, cmt) => Hashtbl.mem(state.cmtCache, cmt);

let docsForCmt = (cmt, src, state) =>
  if (Hashtbl.mem(state.cmtCache, cmt)) {
    let (mtime, infos, docs) = Hashtbl.find(state.cmtCache, cmt);
    /* TODO I should really throttle this mtime checking to like every 50 ms or so */
    switch (Files.getMtime(cmt)) {
    | None => None
    | Some(changed) =>
      if (changed > mtime) {
        newDocs(state.cmtCache, changed, cmt, src);
      } else {
        Some(docs);
      }
    };
  } else {
    switch (Files.getMtime(cmt)) {
    | None => None
    | Some(changed) => newDocs(state.cmtCache, changed, cmt, src)
    };
  };

let updateContents = (uri, text, version, state) => {
  Hashtbl.remove(state.compiledDocuments, uri);
  Hashtbl.replace(state.documentText, uri, (text, int_of_float(version), false));
  state
};

let getCompilationResult = (uri, state) => {
  if (Hashtbl.mem(state.compiledDocuments, uri)) {
    Hashtbl.find(state.compiledDocuments, uri)
  } else {
    let (text, _, _) = Hashtbl.find(state.documentText, uri);
    let result = AsYouType.process(text, state.rootPath, state.includeDirectories, state.compilationFlags);
    Hashtbl.replace(state.compiledDocuments, uri, result);
    result
  }
};

let getDefinitionData = (uri, state) => switch (getCompilationResult(uri, state)) {
| Success(_, _, data) | TypeError(_, _, data) => Some(data)
| _ => None
};

let docsForModule = (modname, state) =>
  Infix.(
    if (Hashtbl.mem(state.pathsForModule, modname)) {
      let (cmt, src) = Hashtbl.find(state.pathsForModule, modname);
      docsForCmt(cmt, src, state) |?>> d => (d, src)
    } else {
      None;
    }
  );

let maybeFound = Definition.maybeFound;

open Infix;

let resolveDefinition = (uri, defn, state) =>
  switch defn {
  | `Local((_, loc, _, docs)) => Some((loc, docs, uri))
  | `Global(top, children) =>
    switch (
      maybeFound(List.assoc(top), state.localModules)
      |?> (
        ((cmt, src)) => {
          let uri = "file://" ++ Infix.fileConcat(state.rootPath, src);
          /* Log.log("Got it! " ++ uri);
          Hashtbl.iter((k, _) => Log.log(k), state.compiledDocuments); */
          maybeFound(Hashtbl.find(state.compiledDocuments), uri)
          |?> AsYouType.getResult
          |?>> (defn => (defn, uri));
        }
      )
    ) {
    | Some(((cmtInfos, data), uri)) =>
      Definition.resolvePath(data, children) |?> ((_, loc, _, docs)) => Some((loc, docs, uri))
    | None =>
      maybeFound(Hashtbl.find(state.pathsForModule), top)
      |?> (
        ((cmt, src)) => {

          let uri = "file://" ++ src;
          /* Log.log("Got it! " ++ uri); */
          /* TODO */
          docsForCmt(cmt, src, state) |?>> snd |?> Docs.findPath(children) |?>> ((name, loc, docs, _)) => (loc, docs, uri)
          /* None */
        }
      )
    }
  };

let getResolvedDefinition = (uri, defn, data, state) => {
  Definition.findDefinition(defn, data) |?> resolveDefinition(uri, _, state)
};

let definitionForPos = (uri, pos, data, state) =>
  Definition.locationAtPos(pos, data)
  |?> (((_, _, defn)) => getResolvedDefinition(uri, defn, data, state));