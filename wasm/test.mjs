// Checks of the browser build under Node (CI's wasm job):
//   node wasm/test.mjs layouts BUILD OUT     the layout hashes of the seed
//                                            on standard input, to OUT
//   node wasm/test.mjs restore BUILD SAVE NAME
//                                            restores SAVE, a gzipped save
//                                            file of player NAME written by
//                                            a native build, and requires
//                                            the game to welcome NAME back
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [cmd, build, ...rest] = process.argv.slice(2);
const { default: NetHack } = await import(pathToFileURL(path.resolve(build, 'nethack.mjs')).href);
// (the data file is found by locateFile, not beside the module)
const locateFile = name => path.resolve(build, name);
const fail = why => {
  console.error(`wasm/test.mjs ${cmd}: ${why}`);
  process.exit(1);
};

if (cmd === 'layouts') {
  const [out] = rest;
  const seed = fs.readFileSync(0);
  let at = 0;
  let errors = '';
  const game = await NetHack({
    locateFile,
    stdin: () => (at < seed.length ? seed[at++] : null),
    printErr: s => {
      errors += s + '\n';
    },
    nhKey: async () => -1,
    onExit: code => {
      if (code !== 0) fail(`exit ${code}: ${errors}`);
      fs.writeFileSync(out, game.FS.readFile('/tmp/layouts.jsonl'));
    },
  });
  game.callMain(['--layout-hashes', '/tmp/layouts.jsonl']);
} else if (cmd === 'restore') {
  const [save, name] = rest;
  let shown = '';
  const game = await NetHack({
    locateFile,
    preRun: [
      m => {
        m.ENV.TERM = 'xterm';
        m.ENV.USER = name;
        m.TTY.default_tty_ops.put_char = (_tty, c) => {
          if (c !== null) shown += String.fromCharCode(c);
        };
        m.TTY.default_tty_ops.fsync = () => {};
      },
    ],
    // the first key the restored game waits for: it has said what it says
    nhKey: async () => {
      const text = shown.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, ' ');
      if (!text.includes(`welcome back`)) fail(`not restored: ${text.slice(-600)}`);
      console.log(`restored ${save} as ${name}: ${text.match(/[^ ]*welcome back[^!]*!/)[0]}`);
      process.exit(0);
    },
    onExit: code => fail(`the game exited (${code}): ${shown.slice(-600)}`),
  });
  game.FS.mkdirTree('/nethack/save');
  game.FS.writeFile(`/nethack/save/0${name}.gz`, fs.readFileSync(save));
  game.callMain(['-u', name]);
} else {
  fail('usage: node wasm/test.mjs layouts BUILD OUT | restore BUILD SAVE NAME');
}
