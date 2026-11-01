"""Execute the actual compiled AArch64 helper with native UI/stat oracles."""
import importlib.util
import struct
import unittest
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from test_heal_probe import probe, relocated

spec=importlib.util.spec_from_file_location('companion',Path(__file__).resolve().parents[1]/'gen_companion_helper.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
MAIN,CODE,MAIL=0x80000000,0x89000000,0x8a000000
APP,MENUS,UI,ROOTSLATE,ROOTCTRL,TREE,PANEL,SLOT,CANVAS,SLATE,CTRL,MM,RES=[0x91000000+i*4096 for i in range(13)]
CHUNKS,CHUNK,NAMES,NODES=0x92000000,0x92100000,0x92200000,0x92300000
BM,FRONT,BACK,ENEMIES,ACTOR,FOE=0x93000000,0x93002000,0x93003000,0x93004000,0x93005000,0x93006000

class HelperTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.body=helper.compile_body()
 def setUp(self):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.pages=set();self.calls=[];self.ticks=0
  def page(p):
   p&=~4095
   if p not in self.pages:self.u.mem_map(p,4096);self.pages.add(p)
  self.page=page
  def raw(p,b):
   for at in range(p&~4095,(p+len(b)+4095)&~4095,4096):page(at)
   self.u.mem_write(p,b)
  self.raw=raw
  self.put=lambda p,v,f='<I':raw(p,struct.pack(f,v))
  self.get=lambda p,f='<I':struct.unpack(f,self.u.mem_read(p,struct.calcsize(f)))[0]
  def obj(p,vt,index):
   self.put(p,MAIN+vt,'<Q');self.put(p+8,8);self.put(p+12,index)
   self.put(CHUNK+index*20,p,'<Q');self.put(CHUNK+index*20+8,0);self.put(CHUNK+index*20+16,1000+index)
  self.obj=obj
  self.put(MAIN+0x67d0420,MAIN+0x6ce97e8,'<Q')
  self.put(MAIN+0x6ce97e8+0x10,CHUNKS,'<Q');self.put(MAIN+0x6ce97e8+0x24,32);self.put(CHUNKS,CHUNK,'<Q')
  self.put(MAIN+0x7cc8cc0,APP,'<Q');obj(APP,0x6207610,10);self.put(APP+0x2f0,23,'<B')
  self.put(MAIN+0x7cc9a18,MENUS,'<Q');obj(MENUS,0x63b6f18,11)
  self.put(MENUS+0x2f0,NODES,'<Q');self.put(MENUS+0x2f8,1);self.put(MENUS+0x2fc,1)
  self.put(MENUS+0x310,0,'<Q');self.put(MENUS+0x318,1);self.put(MENUS+0x324,0);self.put(MENUS+0x330,0,'<Q');self.put(MENUS+0x338,1)
  self.put(MENUS+0x300,1);self.put(MENUS+0x328,0);self.put(NODES,10,'<B');self.put(NODES+8,UI,'<Q');self.put(NODES+16,0xffffffff)
  obj(UI,0x63c0c48,1);self.put(UI+0x108,ROOTSLATE,'<Q');self.put(UI+0x110,ROOTCTRL,'<Q')
  self.put(ROOTSLATE,MAIN+0x5d011e8,'<Q');self.put(ROOTSLATE+8,ROOTSLATE,'<Q');self.put(ROOTCTRL+8,1)
  self.put(UI+0xdc,4,'<B');self.put(ROOTSLATE+0x184,0x11,'<B');self.put(ROOTSLATE+0x185,1,'<B')
  self.put(UI+0x278,TREE,'<Q');self.put(UI+0x488,PANEL,'<Q');obj(TREE,0x5d0ee88,12);self.put(TREE+0x20,UI,'<Q');self.put(TREE+0x30,PANEL,'<Q')
  obj(PANEL,0x5cc23b0,13);self.put(PANEL+0x20,TREE,'<Q');self.put(PANEL+0xdc,0,'<B');self.put(PANEL+0x178,NODES+0x100,'<Q');self.put(PANEL+0x180,1);self.put(PANEL+0x184,1);self.put(NODES+0x100,SLOT,'<Q')
  obj(SLOT,0x5cc35a8,14);self.put(SLOT+0x28,PANEL,'<Q');self.put(SLOT+0x30,CANVAS,'<Q');obj(CANVAS,0x5cc23b0,2)
  self.put(CANVAS+0x20,TREE,'<Q');self.put(CANVAS+0x30,SLOT,'<Q');self.put(CANVAS+0x18,0x40010,'<Q')
  self.put(MAIN+0x6c91478+4*8,NAMES,'<Q');name=b'CanvasPanel_MinimapSmall';self.put(NAMES+32,len(name)<<6,'<H');raw(NAMES+34,name)
  for off,p in [(0x190,SLATE),(0x198,CTRL),(0xf8,SLATE),(0x100,CTRL),(0x108,0),(0x110,0)]:self.put(CANVAS+off,p,'<Q')
  self.put(SLATE,MAIN+0x5b5af90,'<Q');self.put(SLATE+8,SLATE,'<Q');self.put(CTRL+8,1);self.put(SLATE+0x184,0x11,'<B');self.put(SLATE+0x185,1,'<B');self.put(CANVAS+0xdc,4,'<B')
  # Native conversion constants used by UWidget::SetVisibility.
  self.put(MAIN+0x6cf09dc,0x19,'<B')
  for i,(target,value) in enumerate([(0x6cf09e0,2),(0x6cf0934,4),(0x6cf0930,1),(0x6cf09e4,0x11)]):
   self.put(MAIN+0x5cfa588+i*8,MAIN+target,'<Q');self.put(MAIN+target,value,'<B')
  self.put(MAIN+0x7cc7588,MM,'<Q');obj(MM,0x63be0f0,15);self.put(MM+0x448,RES,'<Q');self.put(MM+0x348,46);self.put(MM+0x7b0,0,'<B');self.put(RES,0x479629,'<Q');self.put(RES+0xf8,1,'<B')
  self.put(MAIN+0x7cc5b78,BM,'<Q');self.put(BM+8,8)
  for off,data,count in [(0x9b8,FRONT,1),(0x9c8,BACK,0),(0xa98,ENEMIES,1)]:
   self.put(BM+off,data,'<Q');self.put(BM+off+8,count);self.put(BM+off+12,count)
  self.put(FRONT,ACTOR,'<Q');self.put(ENEMIES,FOE,'<Q')
  for actor,team,id,index in [(ACTOR,0,904,3),(FOE,1,32312,4)]:
   obj(actor,0x63f39b8,index);self.put(actor+0x293,team,'<B');self.put(actor+0x294,0,'<B');self.put(actor+0x298,id);self.put(actor+0x2a7,0,'<B');self.put(actor+0x2a8,1,'<B')
   self.put(actor+0x800,0,'<Q');self.put(actor+0x808,0);self.put(actor+0x80c,0)
  for address in [CODE,MAIL,MAIN+probe.HOOK,MAIN+probe.TICK,MAIN+0x1f135f0,MAIN+0x45bb64,MAIN+0x45bdc0,MAIN+0x3837718,MAIN+0x38308b4,MAIN+0x382e264,0x90000000]:page(address)
  page(CODE+4096);page(CODE+8192)
  p=probe.generate(self.body);targets=dict(main=MAIN,code=CODE,mailbox=MAIL)
  raw(CODE,relocated(bytes.fromhex(p['guest_code']['bytes']),p['guest_code']['relocations'],CODE,targets))
  patch=p['patches'][0];raw(MAIN+probe.HOOK,relocated(bytes.fromhex(patch['replacement']),patch['relocations'],MAIN+probe.HOOK,targets))
  self.u.reg_write(UC_ARM64_REG_SP,0x90000ff0)
  self.u.reg_write(UC_ARM64_REG_X19,0x12345678);self.u.reg_write(UC_ARM64_REG_X20,0x23456789)
  self.put(MAIL+4,777)
  def hook(u,pc,size,data):
   if pc==MAIN+probe.TICK:self.ticks+=1;u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x45bb64:
    out=u.reg_read(UC_ARM64_REG_X0);actor=u.reg_read(UC_ARM64_REG_X1);index=self.get(actor+12);item=CHUNK+index*20
    number=self.get(item+16)
    if not number:number=2000+index;self.put(item+16,number)
    self.raw(out,struct.pack('<II',index,number));self.calls.append(('weak',actor));u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x45bdc0:
    out=u.reg_read(UC_ARM64_REG_X0);index,number=struct.unpack('<II',u.mem_read(out,8));item=CHUNK+index*20
    u.reg_write(UC_ARM64_REG_X0,self.get(item,'<Q') if number==self.get(item+16) else 0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x1f135f0:
    actor=u.reg_read(UC_ARM64_REG_X0);value=u.reg_read(UC_ARM64_REG_W1);self.calls.append(('visibility',actor,value))
    self.put(actor+0xdc,value,'<B');self.put(SLATE+0x184,[0x19,2,4,1,0x11][value],'<B');u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x3837718:u.reg_write(UC_ARM64_REG_W0,1 if u.reg_read(UC_ARM64_REG_X0)==ACTOR else 0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x38308b4:self.calls.append(('clear',u.reg_read(UC_ARM64_REG_X0)));u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   elif pc==MAIN+0x382e264:
    actor=u.reg_read(UC_ARM64_REG_X0);parameter=u.reg_read(UC_ARM64_REG_W1);self.calls.append(('get',actor,parameter))
    u.reg_write(UC_ARM64_REG_W0,100+parameter);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
  self.u.hook_add(UC_HOOK_CODE,hook)
 def tick(self):
  self.u.reg_write(UC_ARM64_REG_X0,0xabcdef)
  self.u.emu_start(MAIN+probe.HOOK,MAIN+probe.HOOK+4,count=1000000)
  self.assertEqual(self.u.reg_read(UC_ARM64_REG_X19),0x12345678)
  self.assertEqual(self.u.reg_read(UC_ARM64_REG_X20),0x23456789)
 def lease(self,desired=1):
  seq=(self.get(MAIL+20)+2)&~1
  self.put(MAIL+20,seq-1)
  for at,value,fmt in [(8,desired,'<I'),(12,self.get(MAIL)+120,'<I'),(16,777,'<I'),(128,RES,'<Q'),(136,0x479629,'<Q'),(144,46,'<I'),(192,CANVAS,'<Q'),(200,self.get(CHUNK+2*20+16),'<I'),(204,2,'<I'),(208,UI,'<Q'),(216,self.get(CHUNK+1*20+16),'<I'),(220,1,'<I')]:self.put(MAIL+at,value,fmt)
  self.put(MAIL+20,seq)
 def test_native_visibility_restore_and_original_tick(self):
  self.lease();self.tick();self.assertEqual(self.get(CANVAS+0xdc,'<B'),1);self.assertEqual(self.get(MAIL+28),1)
  self.lease(0);self.tick();self.assertEqual(self.get(CANVAS+0xdc,'<B'),4);self.assertEqual(self.get(MAIL+28),0);self.assertEqual(self.ticks,2)
 def test_section_change_expiry_and_torn_lease(self):
  self.lease();self.tick();self.put(MAIL+20,3);self.put(MAIL+136,999,'<Q');self.tick();self.assertEqual(self.get(MAIL+28),1)
  self.put(MAIL,self.get(MAIL)+121);self.tick();self.assertEqual(self.get(CANVAS+0xdc,'<B'),4)
  self.lease();self.tick();self.put(RES,123,'<Q');self.tick();self.assertEqual(self.get(CANVAS+0xdc,'<B'),4)
 def test_no_unknown_or_native_hidden_takeover(self):
  self.lease();self.put(CANVAS+0xdc,2,'<B');self.put(SLATE+0x184,4,'<B');self.tick();self.assertFalse(self.calls)
 def test_native_weak_reference_initializes_zero_canvas_serial(self):
  self.put(CHUNK+2*20+16,0);self.tick()
  self.assertEqual(self.get(CHUNK+2*20+16),2002)
  self.assertEqual(self.get(CANVAS+0xdc,'<B'),4)
  self.lease();self.tick();self.assertEqual(self.get(MAIL+28),1)
 def test_native_weak_reference_refuses_wrong_global_allocator(self):
  self.put(CHUNK+2*20+16,0);self.put(MAIN+0x67d0420,0,'<Q');self.lease();self.tick()
  self.assertEqual(self.get(CHUNK+2*20+16),0);self.assertFalse(self.calls)
 def test_effect_free_fresh_parameter_query(self):
  self.put(APP+0x2f0,26,'<B');self.put(MAIL+72,1);self.put(MAIL+80,777);self.put(MAIL+84,120);self.put(MAIL+92,(1<<0)|(1<<8));self.put(MAIL+72,2)
  self.tick();self.assertEqual(self.get(MAIL+76),2);self.assertEqual(self.get(MAIL+88),1)
  self.assertEqual(len([x for x in self.calls if x[0]=='clear']),2);self.assertEqual(len([x for x in self.calls if x[0]=='get']),20)
  for slot in [0,8]:
   row=MAIL+256+slot*80;self.assertEqual(self.get(row+20),255);self.assertEqual(self.get(row+64,'<Q'),0x4d25767f9dce13f5)
   self.assertEqual(self.get(row+24),109)
 def test_query_warms_zero_actor_serial_without_resource_setters(self):
  self.put(CHUNK+3*20+16,0);self.put(APP+0x2f0,26,'<B')
  self.put(MAIL+80,777);self.put(MAIL+84,120);self.put(MAIL+92,1);self.put(MAIL+72,2)
  self.tick();self.assertEqual(self.get(MAIL+88),1)
  self.assertEqual(self.get(MAIL+256+60),2003)
  self.assertEqual(self.get(MAIL+256+20),255)
  self.assertTrue(('weak',ACTOR) in self.calls)
 def test_query_refuses_epoch_state_and_torn_publication(self):
  self.put(MAIL+72,1);self.put(MAIL+80,777);self.put(MAIL+84,120);self.put(MAIL+92,1);self.tick();self.assertEqual(self.get(MAIL+76),0)
  self.put(MAIL+72,2);self.tick();self.assertEqual(self.get(MAIL+88),4);self.assertFalse(self.calls)

if __name__=='__main__':unittest.main()
