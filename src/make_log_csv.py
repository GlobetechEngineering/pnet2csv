import sys
import struct

def parse_timestamp(PLCts, endian):
    days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat']
    return {
        'Y': int.from_bytes(PLCts[0:2], byteorder=endian),
        'M': PLCts[2],
        'D': PLCts[3],
        'h': PLCts[5],
        'm': PLCts[6],
        's': PLCts[7],
        'n': int.from_bytes(PLCts[8:12], byteorder=endian),
        'w': days[PLCts[4] - 1] # Weekday counts from 1 = Sunday
    }
    
def parse_typelist(typelist):
    '''
    typelist: a string of characters specifying types, with optional width digits before them
    returns: a list of tuples, whose first element is the type specififier,
        and whose second element is a slice object denoting the span of this variable in bytes
    example: parse_typelist('bb2dd4f') =>
        [('b',0:2), ('b',2:4), ('d',4:8), ('d',8:10), ('f',10:18)]
    except as actual objects rather than syntax errors
    '''
    types = []
    # i: how many bytes have we spanned so far
    i = 0
    width = 2
    if isinstance(typelist, bytes):
        # decode to str, for isdecimal
        # ascii will throw on >= 128, which is probably alright
        typelist = typelist.decode('ascii')
    for char in typelist:
        if char == '\x00':
            continue
        if char.isdecimal():
            if width > 2:
                # already saw a digit. Are widths > 9 supported or should we assume a type?
                # assume an interluding x and continue
                types.append('x', (slice(i,i+width)))
                i += width
            width = 2*int(char)
            continue
        # not our business to know valid types
        types.append((char, slice(i,i+width)))
        i += width
        width = 2
    return types

def die(msg):
    print(msg, file=sys.stderr)
    exit(1)

if len(sys.argv) < 2:
    die("No file provided")

in_name = sys.argv[1]
bin_ext = in_name.rfind('.bin')
if bin_ext == -1:
    out_name = in_name + '.csv'
else:
    out_name = in_name[0:bin_ext] + '.csv'

try:
    with open(in_name, 'rb') as in_file:
        magic = in_file.read(4)
        if magic != b'\x61\x0b\xe7\xec':
            die(f"{in_file.name} is not a valid log file")
            
        endianflag = in_file.read(3)
        # these names are passed directly to int.from_bytes
        if endianflag == b'PNL':
            endian = 'big'
        elif endianflag == b'LNP':
            endian = 'little'
        else:
            die(f"Invalid endian flag: {endianflag}")
        
        version = int.from_bytes(in_file.read(1))
        if version > 0:
            print(f"Warning: unsupported log version {version}", file=sys.stderr)
        
        log_id = in_file.read(8)
        
        word_count = int.from_bytes(in_file.read(1))
        
        type_list = in_file.read(word_count)
        types = parse_typelist(type_list)
        
        # No need to stop the world - could just warn,
        # assume x for underspecified and ignore overspecified
        if types[-1][1].stop != 2*word_count:
            die(f"Type list {type_list} spans {types[-1][1].stop / 2} words (expected {word_count})")
        type_header = ','.join([f'{type[0]}:{type[1].start}' for type in types])
        
        with open(out_name, 'w') as out_file:
            out_file.write(f"\"{log_id.decode()}\"\n")
            
            out_file.write(f"Day,Date,Time,{type_header}\n")
            
            EOF = False
            while not EOF:
                start_byte = in_file.read(1)
                match start_byte:
                    case b'\x00':
                        # start of another entry
                        pass
                    case b'\xFF':
                        # No more entries
                        EOF = True # this doesn't actually do anything
                        break
                    case b'':
                        print(f"Warning: {in_name} is missing entries", file=sys.stderr)
                        break
                    case _:
                        die(f"Unexpected byte at {in_file.tell()-1}")
                
                timestamp = in_file.read(12)
                if len(timestamp) != 12:
                    die(f"{in_name} is incomplete")

                ts_dict = parse_timestamp(timestamp, endian)
                ts_str = "{0[w]},{0[Y]:4}-{0[M]:02}-{0[D]:02},{0[h]:02}:{0[m]:02}:{0[s]:02}.{0[n]:09}".format(ts_dict)
                
                word_bytes = in_file.read(2*word_count)
                if len(word_bytes) != 2*word_count:
                    die(f"{in_name} is incomplete")

                vals = [0]*len(types)
                for i,type in enumerate(types):
                    value_bytes = word_bytes[type[1]]
                    width = len(value_bytes)
                    match type[0]:
                        case 'u':
                            val = int.from_bytes(value_bytes, byteorder=endian, signed=False)
                            vals[i] = str(val)
                        case 'd':
                            val = int.from_bytes(value_bytes, byteorder=endian, signed=True)
                            vals[i] = str(val)
                        case 'f':
                            structend = {'big': '>', 'little': '<'}
                            structtype = {2: 'e', 4: 'f', 8: 'F'}
                            val, = struct.unpack(structend[endian]+structtype[width], value_bytes)
                            vals[i] = '{:.8g}'.format(val)
                        case 'b':
                            val = int.from_bytes(value_bytes, byteorder=endian, signed=False)
                            size = 10*width - 1 # accounting for _ every four bits
                            vals[i] = '{0:0{length}_b}'.format(val, length=size)
                        case 'x' | _:
                            vals[i] = value_bytes.hex(' ',2)
                
                val_str = ','.join(vals)
                
                out_file.write(f"{ts_str},{val_str}\n")
        print(f"Finished writing {out_name}")
except OSError as err:
    die(f"{err.filename}: {err.strerror}")
