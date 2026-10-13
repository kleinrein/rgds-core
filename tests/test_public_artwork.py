import io
import sys
import tempfile
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
import public_artwork as art
import scraper
from library import game_key, read_fields, write_fields

class PublicArtworkTests(unittest.TestCase):
    def test_header_identity_raw_and_zip_without_full_rom_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'Mario.nds';path.write_bytes(b'\0'*12+b'ASME'+b'\0'*500)
            self.assertEqual(art.rom_details(path),('Mario','ASME'))
            packed=path.with_suffix('.zip')
            with zipfile.ZipFile(packed,'w') as archive:archive.write(path,'folder/Mario.nds')
            self.assertEqual(art.rom_details(packed),('Mario','ASME'))
            with zipfile.ZipFile(packed,'a') as archive:archive.writestr('other.nds',b'')
            with self.assertRaises(scraper.NotFound):art.rom_details(packed)

    def test_gametdb_uses_exact_code_and_native_region(self):
        client=art.PublicClient('gametdb')
        with patch.object(client,'fetch',return_value=b'png') as fetch:
            raw,meta=client.gametdb('Mario','ASME')
        fetch.assert_called_once_with('https://art.gametdb.com/ds/coverS/US/ASME.png')
        self.assertEqual(meta['provider'],'GameTDB')
        self.assertNotIn('title',meta)

    def test_renamed_libretro_match_retains_sequel_number_and_region(self):
        client=art.PublicClient('libretro')
        client.index=['Puzzle 2 (USA)','Puzzle (USA)','Puzzle (Europe)']
        with patch.object(client,'fetch',side_effect=[scraper.NotFound(),b'png']) as fetch:
            _,meta=client.libretro('Puzzle','ABCE')
        self.assertEqual(meta['provider_id'],'Puzzle (USA)')
        self.assertIn('Puzzle%20%28USA%29.png',fetch.call_args.args[0])
        self.assertNotEqual(art.title_key('Puzzle 2'),art.title_key('Puzzle'))

    def test_auto_falls_back_without_credentials(self):
        client=art.PublicClient()
        with patch.object(art,'rom_details',return_value=('Puzzle','ABCE')),patch.object(client,'gametdb',side_effect=scraper.NotFound()),patch.object(client,'libretro',return_value=(b'png',{'provider':'Libretro'})):
            self.assertEqual(client.artwork(Path('game.nds'))[1]['provider'],'Libretro')

    def test_public_redirect_cannot_exfiltrate_or_downgrade(self):
        request=art.urllib.request.Request('https://art.gametdb.com/ds/a.png')
        for url in ('http://art.gametdb.com/a','https://evil.example/a','https://thumbnails.libretro.com/a','https://art.gametdb.com:444/a'):
            with self.assertRaises(scraper.ScraperError):art.PublicRedirect().redirect_request(request,None,302,'',{},url)

    def test_fetch_enforces_size_limit_and_safe_final_host(self):
        class Response(io.BytesIO):
            def geturl(self):return 'https://art.gametdb.com/ds/a.png'
        client=art.PublicClient(opener=lambda *a,**k:Response(b'12345'))
        with self.assertRaisesRegex(scraper.ScraperError,'size limit'):client.fetch('https://art.gametdb.com/ds/a.png',4)

    def test_public_scrape_keeps_richer_metadata_and_existing_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);rom=data/'Mario.nds';rom.write_bytes(b'\0'*12+b'ASME')
            item=data/'library'/game_key(rom);write_fields(item/'info.ini',{'title':'Mario','year':'2006'})
            client=art.PublicClient()
            def image(raw,destination):destination.write_bytes(raw)
            with patch.object(client,'artwork',return_value=(b'bmp',{'provider':'GameTDB'})) as fetch,patch.object(scraper,'image_to_bmp',side_effect=image):
                scraper.scrape(data,client,[rom]);scraper.scrape(data,client,[rom]);fetch.assert_called_once()
            self.assertEqual(read_fields(item/'info.ini')['year'],'2006')
            self.assertEqual(read_fields(data/'scraper/status.ini')['state'],'done')

    def test_transient_service_error_retries_once_and_outage_cools_down(self):
        class Response(io.BytesIO):
            def geturl(self):return 'https://art.gametdb.com/ds/a.png'
        error=urllib.error.HTTPError('https://art.gametdb.com/ds/a.png',503,'unavailable',None,None)
        client=art.PublicClient()
        with patch.object(client,'opener',side_effect=[error,Response(b'png')]) as opener,patch.object(art.time,'sleep'):
            self.assertEqual(client.fetch('https://art.gametdb.com/ds/a.png'),b'png')
            self.assertEqual(opener.call_count,2)
        with patch.object(client,'opener',side_effect=error) as opener,patch.object(art.time,'sleep'):
            with self.assertRaises(scraper.ScraperError):client.fetch('https://art.gametdb.com/ds/a.png')
            with self.assertRaises(scraper.ScraperError):client.fetch('https://art.gametdb.com/ds/another.png')
            self.assertEqual(opener.call_count,2)

    def test_stale_catalog_remains_usable_when_server_is_unavailable(self):
        import os
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'ds-serials.dat';path.write_text('game (\n name "Puzzle (Europe)"\n serial "ABCP"\n)\n')
            os.utime(path,(1,1));client=art.PublicClient(cache=folder)
            with patch.object(client,'fetch',side_effect=scraper.ScraperError('Offline')):
                self.assertEqual(client.names_for_code('ABCP'),['Puzzle (Europe)'])

    def test_code_catalog_recovers_a_renamed_rom_without_fuzzy_guessing(self):
        client=art.PublicClient('libretro')
        client.index=['Puzzle (Europe)', 'Puzzle 2 (Europe)']
        client.serials={'ABCP':['Puzzle (Europe) (En,Fr)']}
        with patch.object(client,'fetch',side_effect=[scraper.NotFound(),b'png']):
            _,meta=client.libretro('my favourite cartridge','ABCP')
        self.assertEqual(meta['provider_id'],'Puzzle (Europe)')
        self.assertEqual(meta['artwork_match'],'game_code')

    def test_same_serial_with_distinct_titles_is_not_guessed(self):
        client=art.PublicClient('libretro');client.index=['Puzzle (Europe)','Puzzle 2 (Europe)']
        client.serials={'ABCP':client.index}
        with patch.object(client,'fetch',side_effect=scraper.NotFound()):
            with self.assertRaisesRegex(scraper.NotFound,'More than one'):client.libretro('renamed','ABCP')

    def test_manual_title_does_not_reuse_a_wrong_header_code(self):
        client=art.PublicClient()
        with patch.object(art,'rom_details',return_value=('renamed','ABCP')),patch.object(client,'libretro',return_value=(b'png',{})) as fetch:
            client.artwork('rom.nds','Puzzle 2')
        fetch.assert_called_once_with('Puzzle 2','')

    def test_title_normalization_preserves_distinct_titles_and_nordic_letters(self):
        self.assertEqual(art.title_key('Flå klypa (Europe) (En,Fr)'),art.title_key('Fla klypa'))
        self.assertNotEqual(art.title_key('Puzzle (Advance)'),art.title_key('Puzzle'))
        self.assertNotEqual(art.title_key('Puzzle 2'),art.title_key('Puzzle'))
        self.assertEqual(art.display_title('0123 - Sims 2, The (Europe) (Rev 1)'), 'The Sims 2')

    def test_bad_game_and_service_failure_do_not_abort_remaining_games(self):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);roms=[data/(n+'.nds') for n in ('broken','offline','missing','good')]
            for rom in roms:rom.write_bytes(b'header')
            client=art.PublicClient()
            def image(raw,path):path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
            failures=[zipfile.BadZipFile(),scraper.ScraperError('Wi-Fi failed'),scraper.NotFound('No cover'),(b'bmp',{'provider':'Libretro'})]
            with patch.object(client,'artwork',side_effect=failures),patch.object(scraper,'image_to_bmp',side_effect=image):
                scraper.scrape(data,client,roms)
            status=read_fields(data/'scraper/status.ini')
            self.assertEqual((status['completed'],status['failed'],status['missing'],status['downloaded']),('4','2','1','1'))
            self.assertEqual(read_fields(data/'library'/game_key(roms[-1])/'artwork-status.ini')['state'],'downloaded')
            self.assertTrue((data/'scraper/report.json').is_file())

    def test_failed_forced_refresh_retains_existing_art(self):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);rom=data/'rom.nds';rom.write_bytes(b'header')
            item=data/'library'/game_key(rom);item.mkdir(parents=True);(item/'box.bmp').write_bytes(b'old')
            client=art.PublicClient()
            with patch.object(client,'artwork',side_effect=scraper.ScraperError('Offline')):scraper.scrape(data,client,[rom],True)
            self.assertEqual((item/'box.bmp').read_bytes(),b'old')
            self.assertEqual(read_fields(item/'artwork-status.ini')['state'],'error')

    def test_catalog_cache_can_work_offline_and_is_reused(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'ds-serials.dat'
            path.write_text('game (\n name "Puzzle (Europe)"\n serial "ABCP"\n)\n')
            client=art.PublicClient(cache=folder)
            with patch.object(client,'fetch',side_effect=AssertionError('network')):
                self.assertEqual(client.names_for_code('ABCP'),['Puzzle (Europe)'])

    def test_cancellation_preserves_partial_counts_and_report(self):
        with tempfile.TemporaryDirectory() as folder:
            data=Path(folder);roms=[data/'first.nds',data/'second.nds'];client=art.PublicClient()
            with patch.object(client,'artwork',side_effect=[scraper.NotFound('No cover'),KeyboardInterrupt()]):scraper.scrape(data,client,roms)
            status=read_fields(data/'scraper/status.ini')
            self.assertEqual(status['state'],'cancelled');self.assertEqual(status['completed'],'1')
            self.assertEqual(status['missing'],'1')

if __name__=='__main__':unittest.main()
