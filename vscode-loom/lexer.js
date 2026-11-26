'use strict';

const KEYWORDS = new Set([
  'Module','Block','Global','Takes','Produces','Declares','Implements','End','Give','If','Else',
  'Loop','While','while','Until','until','Exit','Skip','Ensure','Match','match','When','when','Import','Use','use',
  'Default','default','Where','where','In','in','Move','move','Copy','copy','Borrow','borrow',
  'Constant','Record','Enum','Extern','Packed','packed','Align','align','Address','Deref','Cast','Slice','Null',
  'True','False','And','Or','or','Not','Mod','LSL','LSR','Spawn','Join','Cancel','Allocate',
  'Resize','Release','Try','try','Test','As','as','Pow','pow','Volatile','volatile',
  'Within','Vol_Read_8','Vol_Read_16','Vol_Read_32','Vol_Read_64',
  'Vol_Write_8','Vol_Write_16','Vol_Write_32','Vol_Write_64','Nop','WaitForInterrupt','MemoryBarrier',
  'Pio_Program','Set','Wrap_Target','Wrap','Pull','Push','Out','In','Mov','Jmp','Wait','Irq','Side',
  'Gpio','Pio','Uart','Spi','I2c','Pwm','Adc','Dma','Clocks','Clock','System','Time','Watchdog','Rtc','Sio','Multicore','Usb_Cdc'
]);
const BUILTIN_TYPES = new Set([
  'Signed_8','Signed_16','Signed_32','Signed_64','Unsigned_8','Unsigned_16','Unsigned_32','Unsigned_64',
  'Float_32','Float_64','Boolean','Character','String','Text','String_Builder','File','File_Handle','Task_Handle',
  'Pointer','Block_Pointer','Array','Slice','Vector','HMap','No_Type','Volatile_32','Register_32','Pin','Pio_State_Machine'
]);
const TYPE_ALIASES = new Map();

function tokenize(text) {
  const tokens = [];
  let i = 0, line = 0, character = 0;
  const advance = () => {
    const c = text[i++];
    if (c === '\n') { line++; character = 0; }
    else character++;
    return c;
  };
  while (i < text.length) {
    const c = text[i];
    if (c === ' ' || c === '\t' || c === '\r' || c === '\n') { advance(); continue; }
    if (c === '#' || (c === '/' && text[i + 1] === '/')) {
      while (i < text.length && text[i] !== '\n') advance();
      continue;
    }
    const start = i, startLine = line, startCharacter = character;
    if (c === '"' || c === "'") {
      const quote = advance();
      while (i < text.length) {
        const next = advance();
        if (next === '\\' && i < text.length) { advance(); continue; }
        if (next === quote || next === '\n') break;
      }
      tokens.push({ kind: c === '"' ? 'string' : 'character', value: text.slice(start, i), start, end: i, line: startLine, character: startCharacter });
      continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      advance();
      while (i < text.length && /[A-Za-z0-9_]/.test(text[i])) advance();
      tokens.push({ kind: 'identifier', value: text.slice(start, i), start, end: i, line: startLine, character: startCharacter });
      continue;
    }
    if (/[0-9]/.test(c)) {
      advance();
      while (i < text.length && /[A-Za-z0-9_.]/.test(text[i])) advance();
      tokens.push({ kind: 'number', value: text.slice(start, i), start, end: i, line: startLine, character: startCharacter });
      continue;
    }
    const two = text.slice(i, i + 2);
    if (['..','+=','-=','*=','/=','%=','&=','|=','^=','==','!=','<=','>='].includes(two)) {
      advance(); advance();
      tokens.push({ kind: 'operator', value: two, start, end: i, line: startLine, character: startCharacter });
    } else {
      advance();
      tokens.push({ kind: 'punctuation', value: c, start, end: i, line: startLine, character: startCharacter });
    }
  }
  return tokens;
}

module.exports = { KEYWORDS, BUILTIN_TYPES, TYPE_ALIASES, tokenize };
