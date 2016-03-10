from subprocess import Popen, PIPE
import sys
import re
import datetime

# contains a dictionary of settings for every benchmark test
bench_settings = [
    {
        'name': 'Cornell box',
        'scene': 'scenes/cornell/cornell_org.scene',
        'reference': 'references/ref_cornell_org.png',
        'width': 1024,
        'height': 1024,
        'base_filename': 'results/cornell',
        'args': []
    },

    # {
    #     'name': 'Cornell specular balls',
    #     'scene': 'scenes/cornell/cornell_specular_front.scene',
    #     'reference': 'references/ref_cornell_specular_front.png',
    #     'width': 1024,
    #     'height': 1024,
    #     'base_filename': 'results/cornell_specular_front',
    #     'args': []
    # },

    # {
    #     'name': 'Cornell specular balls close',
    #     'scene': 'scenes/cornell/cornell_specular.scene',
    #     'reference': 'references/ref_cornell_specular.png',
    #     'width': 1024,
    #     'height': 1024,
    #     'base_filename': 'results/cornell_specular',
    #     'args': []
    # },

    # {
    #     'name': 'Cornell indirect',
    #     'scene': 'scenes/cornell/cornell_indirect.scene',
    #     'reference': 'references/ref_cornell_indirect.png',
    #     'width': 1024,
    #     'height': 1024,
    #     'base_filename': 'results/cornell_indirect',
    #     'args': []
    # },

    # {
    #     'name': 'Cornell water',
    #     'scene': 'scenes/cornell/cornell_water.scene',
    #     'reference': 'references/ref_cornell_water.png',
    #     'width': 1024,
    #     'height': 1024,
    #     'base_filename': 'results/cornell_water',
    #     'args': []
    # }
]

alg_small = ['pt', 'bpt', 'vcm']
alg_large = ['pt', 'bpt', 'vcm', 'lt', 'ppm']

time_sec = 1
algorithms = alg_small

def run_benchmark(app, setting):
    results = ''

    for alg in algorithms:
        print '   > running ' + alg + ' ... '

        out_filename = setting['base_filename'] + '_' + alg + '.png'
        p = Popen([app, setting['scene'],
                  '-w', str(setting['width']),
                  '-h', str(setting['height']),
                  '-q', '-t', str(time_sec), '-a', alg,
                  out_filename],
                  stdin=PIPE, stdout=PIPE, stderr=PIPE)

        output, err = p.communicate()

        output_lines = output.splitlines()
        perf_result = output_lines[len(output_lines) - 1]

        print '   > ' + perf_result

        m = re.match(r'Done after (\d+\.\d*) seconds, (\d+) samples @ (\d+\.\d*) frames per second, (\d+\.?\d*)ms per frame', perf_result)

        time = m.group(1)
        samples = m.group(2)
        fps = m.group(3)
        ms_per_frame = m.group(4)

        # Compute RMSE with ImageMagick
        p = Popen(['compare', '-metric', 'RMSE', out_filename, setting['reference'], setting['base_filename'] + '_compare_' + alg + '.png'],
                  stdin=PIPE, stdout=PIPE, stderr=PIPE)
        output, err = p.communicate()

        m = re.match(r'(\d+\.\d*)', err)
        rmse = m.group(1)

        print '   > RMSE: ' + rmse
        print '   > '

        results += setting['name'] + ',' + alg + ',' + time + ',' + samples + ',' + fps + ',' + ms_per_frame + ',' + rmse + '\n'

    return results

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print 'Invalid command line arguments. Expected path to imbatracer executable.'
        quit()

    app = sys.argv[1]

    timestamp = datetime.datetime.now().strftime('%Y_%m_%d_%H_%M_%S')
    res_file = open('results/result_' + timestamp + '.csv', 'w')
    res_file.write('name, algorithm, time (seconds), samples, frames per second, ms per frame, RMSE\n')

    i = 1
    for setting in bench_settings:
        print '== Running benchmark ' + str(i) + ' / ' + str(len(bench_settings)) + ' - ' + setting['name']
        res_file.write(run_benchmark(app, setting))
        i += 1