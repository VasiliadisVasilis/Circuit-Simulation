#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "types.h"
#include "syntactic.tab.h"
#include "solution.h"
#include "csparse.h"

#define  epsilon 2.71828183
#define  pi      3.14159265

int yylex_destroy();
int yyparse();
extern struct components_t *g_components;
extern struct instruction_t *g_instructions;
extern struct option_t *g_options;
int spd_flag = 0;
int iter_type = NoIter;
double itol = 0.001;
int use_sparse = 0;
double *dc_point=NULL;

extern FILE * yyin;

void options_cleanup(struct option_t *g_options);
int circuit_rename_nodes(struct components_t *circuit, struct instruction_t *instr, int element_types[], int **renamed_nodes, int *max_nodes);

enum elementTypes {typeV, typeI, typeR, typeC, typeL, typeM, typeD, typeQ };

double linear_interpolate(double x, double x0, double y0, double x1, double y1)
{
  return y0 + ((x-x0)*y1 - (x-x0)*y0)/(x1-x0);
}

double calculate_ac(const transient_spec_t *transient, double t)
{
  int i;
  if ( transient == NULL ) 
    return 0;

  switch ( transient->type ) {
    case Exp:
      if ( t < transient->exp.td1 ) {
        return transient->exp.i1;
      } else if ( t < transient->exp.td2 ) {
        return transient->exp.i1 + ( transient->exp.i2 - transient->exp.i1 )*
            ( 1 - pow(epsilon, -(t-transient->exp.td1)/transient->exp.tc1));
      } else {
        return transient->exp.i1 + ( transient->exp.i2 - transient->exp.i1 )*
            ( pow(epsilon, -(t-transient->exp.td2)/transient->exp.tc2) 
            - pow(epsilon, -(t-transient->exp.td1)/transient->exp.tc1));
      }
    break;

    case Sin:
      if ( t < transient->_sin.td ) {
        return transient->_sin.i1 + transient->_sin.ia * sin( 2*pi * transient->_sin.ph / 360);
      } else {
        return transient->_sin.i1 + transient->_sin.ia 
          * sin(2*pi * transient->_sin.fr * (t - transient->_sin.td ) 
          + 2*pi * transient->_sin.ph / 360) 
          * pow(epsilon, -(t-transient->_sin.td)* transient->_sin.df);
      }

    break;
    case Pulse:
      if ( t > transient->pulse.per )
        t-= transient->pulse.per;

      if ( t < transient->pulse.td ) {
        return transient->pulse.i1;
      } else if ( t  < transient->pulse.td + transient->pulse.tr ) {
        return  linear_interpolate(t,
                                  transient->pulse.td,
                                  transient->pulse.i1,
                                  transient->pulse.td+transient->pulse.tr,
                                  transient->pulse.i2);
      } else if ( t  < transient->pulse.td + transient->pulse.tr + 
                       transient->pulse.pw) {
        return transient->pulse.i2;
      } else if ( t  < transient->pulse.td + transient->pulse.tr +
                       transient->pulse.pw + transient->pulse.tf ) {
        return linear_interpolate(t,
                                  transient->pulse.td+transient->pulse.tr+transient->pulse.pw,
                                  transient->pulse.i2,
                                  transient->pulse.td+transient->pulse.tr+transient->pulse.pw
                                      + transient->pulse.tf,
                                  transient->pulse.i1);
      } else {
        return transient->pulse.i1;
      }
       
    break;

    case Pwl:
      for ( i=0; i<transient->pwl.size; i++ ) {
        if ( t > transient->pwl.pairs[i].t ) {
          if( i == transient->pwl.size-1 ) {
            return transient->pwl.pairs[i].i;            
          }

          return linear_interpolate(t,
                      transient->pwl.pairs[i].t,
                      transient->pwl.pairs[i].i,
                      transient->pwl.pairs[i+1].t,
                      transient->pwl.pairs[i+1].i);
        }
      }

      return transient->pwl.pairs->i;
    break;

    default:
      printf("Invalid calculate_ac type\n");
      exit(0);
  }
  
}


void instructions_print(struct instruction_t *instr)
{
  int i;
  if ( instr == NULL ) {
    printf("[#] No instructions\n");
    return;
  }
  while ( instr ) {
    switch(instr->type) {
      case Dc:
        printf("DC source: %d[%s] begin: %g end: %g step: %g\n",
            instr->dc.source, (instr->dc.sourceType == Voltage? "Voltage" : "Current" ), instr->dc.begin, instr->dc.end, instr->dc.inc);
        break;

      case Plot:
        printf("PLOT sources: " );
        for ( i=0; i<instr->plot.num-1; i++ )
          printf("%d, ", instr->plot.list[i]);
        printf("%d\n", instr->plot.list[i]);
        break;

      default:
        printf("Unsupported instruction (%d)\n", instr->type);
    }
    instr = instr->next;
  }
}

void options_cleanup(struct option_t *g_options)
{
  struct option_t *o = g_options, *n;

  while ( o ) {
    n = o->next;
    free(o);
    o = n;
  }
}

void instructions_cleanup(struct instruction_t *instr)
{
  struct instruction_t  *next;
  int i =0;

  while ( instr != NULL ) {
    next = instr->next;

    if ( instr->type == Plot ) {
      free( instr->plot.list );
      for (i=0; i< instr->plot.num; i++ )
        fclose(instr->plot.output[i]);
      free( instr->plot.output );
    }

    free(instr);

    instr = next;
  }

}

void circuit_print(struct components_t *circuit)
{
  struct components_t *s;
  s = g_components;

  for (;s!=NULL; s=s->next) {
    switch ( s->data.type ) {
      case V:
        printf("V"); 
        printf("%d +:%d -:%d v:%lf\tis ground:%s\n", s->data.t1.id, s->data.t1.plus, s->data.t1.minus, s->data.t1.val,
            (s->data.t1.val==0 ? "yes" : "no"));
        break;
      case I:
        printf("I");
        printf("%d +:%d -:%d i:%lf\n", s->data.t1.id, s->data.t1.plus, s->data.t1.minus, s->data.t1.val);
        break;
      case R:
        printf("R");
        printf("%d +:%d -:%d r:%lf\n", s->data.t1.id, s->data.t1.plus, s->data.t1.minus, s->data.t1.val);
        break;
      case C:
        printf("C");
        printf("%d +:%d -:%d c:%lf\n", s->data.t1.id, s->data.t1.plus, s->data.t1.minus, s->data.t1.val);
        break;
      case L:
        printf("L");
        printf("%d +:%d -:%d l:%lf\n", s->data.t1.id, s->data.t1.plus, s->data.t1.minus, s->data.t1.val);
        break;
      case D:
        printf("D%d +:%d -:%d model:%s", s->data.t2.id, s->data.t2.plus, s->data.t2.minus, s->data.t2.model_name);
        if ( s->data.t2.area_used )
          printf(" area:%lf\n", s->data.t2.area );
        else
          printf("\n");
        break;
      case Q:
        printf("Q%d c:%d b:%d e:%d model:%s", s->data.t4.id, s->data.t4.c, s->data.t4.b, s->data.t4.e, s->data.t4.model_name);
        if ( s->data.t4.area_used )
          printf(" area:%lf\n", s->data.t4.area );
        else
          printf("\n");
        break;
      case M:
        printf("M%d d:%d g:%d s:%d b:%d l:%lf w:%lf model:%s\n", s->data.t3.id, s->data.t3.d, s->data.t3.g, 
            s->data.t3.s, s->data.t3.b, s->data.t3.l, s->data.t3.w, s->data.t3.model_name);
        break;
      default: printf("unknown: %d !!!\n", s->data.type);
    }
  }
}

void circuit_cleanup(struct components_t *circuit)
{
  struct components_t *e, *p;

  e = circuit;

  while ( e ) {
    switch ( e->data.type ) {
      case D:
        free( e->data.t2.model_name);
        break;
      case M:
        free(e->data.t3.model_name);
        break;
      case Q:
        free(e->data.t4.model_name);
        break;
    }
    p = e->next;
    free(e);
    e = p;
  }

}

void circuit_mna_sparse(struct components_t *circuit, cs** MNA_G, cs** MNA_C, int *max_nodes, int *sources,
    int element_types[], int **renamed_nodes)
{
  int transient_analysis = 0;
  struct components_t *s, *p;
  struct instruction_t *w;
  int max_v_id;
  int  elements, non_zero = 0;
  int x,y,c;

  int inductors = 0;
  // arxika metatrepw tous puknwtes se anoixtokuklwma ( tous afairw apo to kuklwma )
  // kai ta phnia se vraxukuklwma, ta antika8istw me phges tashs 

  s = g_components;

  max_v_id = 1;
  elements = *sources = 0;

  while (s) {
    switch(s->data.type) {
      case V:
        max_v_id = ( s->data.t1.id > max_v_id ? s->data.t1.id : max_v_id );
        if ( s->data.t1.val > 0 )
          (*sources)++;
        non_zero ++;
        break;
      case R:
        if ( s->data.t1.plus == *max_nodes || s->data.t1.minus == *max_nodes)
          non_zero++;
        else
          non_zero+=4;
        elements++;
        break;
      case L:
        inductors++;
        break;
    }

    s = s->next;
  }

  *sources += inductors;
  circuit_rename_nodes(g_components, g_instructions, element_types, renamed_nodes, max_nodes );
  circuit_print(g_components);
  printf("inductors         : %d\n", inductors);
  printf("sources           : %d\n"
      "nodes             : %d\n", *sources-inductors, *max_nodes);
  printf("artificial sources: %d\n", *sources);

  for ( w = g_instructions; w; w=w->next ){
    if ( w->type == Tran ) {
      transient_analysis = 1;
      break;
    }
  }


  *MNA_G = cs_spalloc(*max_nodes+*sources, *max_nodes+ *sources, non_zero, 1,1);

  if ( transient_analysis )
    *MNA_C= cs_spalloc(*max_nodes+*sources, *max_nodes+ *sources, non_zero, 1,1);

  // meta ftiaxnoume to panw aristera elements x elements pou einai ta pa8htika stoixeia
  for (s=circuit; s!=NULL ;s=s->next) {
    if ( s->data.type == R ) {
      if ( s->data.t1.plus < *max_nodes) {
        cs_add_to_entry(*MNA_G, s->data.t1.plus, s->data.t1.plus,1/s->data.t1.val);
      } if ( s->data.t1.minus < *max_nodes ) {
        cs_add_to_entry(*MNA_G, s->data.t1.minus, s->data.t1.minus, 1/s->data.t1.val);
      } if ( s->data.t1.minus < *max_nodes && s->data.t1.plus < *max_nodes ) {
        cs_add_to_entry(*MNA_G, s->data.t1.minus, s->data.t1.plus, -1/s->data.t1.val);
        cs_add_to_entry(*MNA_G, s->data.t1.plus, s->data.t1.minus, -1/s->data.t1.val);
      } 
    } else if ( transient_analysis == 1 && s->data.type == C ) {
      if ( s->data.t1.plus < *max_nodes) {
        cs_add_to_entry(*MNA_C, s->data.t1.plus, s->data.t1.plus, s->data.t1.val);
      } if ( s->data.t1.minus < *max_nodes ) {
        cs_add_to_entry(*MNA_C, s->data.t1.minus, s->data.t1.minus, s->data.t1.val);
      } if ( s->data.t1.minus < *max_nodes && s->data.t1.plus < *max_nodes ) {
        cs_add_to_entry(*MNA_C, s->data.t1.minus, s->data.t1.plus, -s->data.t1.val);
        cs_add_to_entry(*MNA_C, s->data.t1.plus, s->data.t1.minus, -s->data.t1.val);
      } 
    } else if ( transient_analysis && s->data.type == L ) {
      cs_entry(*MNA_C, *max_nodes+*sources-inductors +s->data.t1.id, 
          *max_nodes+*sources-inductors+s->data.t1.id, - s->data.t1.val);
    }

    if ( (s->data.type == V && s->data.t1.val>0) || s->data.type == L ) {
      if ( s->data.t1.plus < *max_nodes ) {
        cs_entry(*MNA_G, *max_nodes + s->data.t1.id, s->data.t1.plus, 1.0 );
        cs_entry(*MNA_G, s->data.t1.plus, *max_nodes + s->data.t1.id, 1.0 );
      }
      if ( s->data.t1.minus < *max_nodes ) {
        cs_entry(*MNA_G, *max_nodes + s->data.t1.id,s->data.t1.minus, -1.0 );
        cs_entry(*MNA_G, s->data.t1.minus, *max_nodes + s->data.t1.id, -1.0 );
      }
    }

  }
  cs_print_formated(*MNA_G, "mna_analysis", *max_nodes+*sources);
  if (transient_analysis)
    cs_print_formated(*MNA_C, "mna_analysis_transient", *max_nodes+*sources);
}

void circuit_mna(struct components_t *circuit, double **MNA_G, double **MNA_C, int *max_nodes, int *sources,
    int element_types[], int **renamed_nodes)
{
  int transient_analysis = 0;
  struct components_t *s, *p;
  struct instruction_t *w;
  int max_v_id;
  int  elements;
  int x,y,c;
  double *matrix, *matrix2;

  int inductors = 0;
  // arxika metatrepw tous puknwtes se anoixtokuklwma ( tous afairw apo to kuklwma )
  // kai ta phnia se vraxukuklwma, ta antika8istw me phges tashs 

  s = g_components;

  max_v_id = 1;
  elements = *sources = 0;

  while (s) {
    switch(s->data.type) {
      case V:
        max_v_id = ( s->data.t1.id > max_v_id ? s->data.t1.id : max_v_id );
        if ( s->data.t1.val > 0 )
          (*sources)++;
        break;
      case R:
        elements++;
        break;
      case L:
        inductors++;
        break;
    }

    s = s->next;
  }

  *sources += inductors;
  circuit_rename_nodes(g_components, g_instructions, element_types, renamed_nodes, max_nodes );
  circuit_print(g_components);
  printf("inductors         : %d\n", inductors);
  printf("sources           : %d\n"
      "nodes             : %d\n", *sources-inductors, *max_nodes);
  printf("artificial sources: %d\n", *sources);

  for ( w = g_instructions; w; w=w->next ){
    if ( w->type == Tran ) {
      transient_analysis = 1;
      break;
    }
  }


  *MNA_G= (double*) calloc((*max_nodes+*sources)*(*max_nodes+*sources), sizeof(double));


  if ( transient_analysis )
    *MNA_C= (double*) calloc((*max_nodes+*sources)*(*max_nodes+*sources), sizeof(double));

  matrix  = *MNA_G;
  matrix2 = *MNA_C;

  // meta ftiaxnoume to panw aristera elements x elements pou einai ta pa8htika stoixeia
  for (s=circuit; s!=NULL ;s=s->next) {
    if ( s->data.type == R ) {
      if ( s->data.t1.plus < *max_nodes)
        matrix[ s->data.t1.plus + s->data.t1.plus*(*max_nodes+*sources)] += 1/s->data.t1.val;
      if ( s->data.t1.minus < *max_nodes )
        matrix[ s->data.t1.minus + s->data.t1.minus*(*max_nodes+*sources)] += 1/s->data.t1.val;
      if ( s->data.t1.minus < *max_nodes && s->data.t1.plus < *max_nodes ) {
        matrix[ s->data.t1.minus + s->data.t1.plus*(*max_nodes+*sources)] -= 1/s->data.t1.val;
        matrix[ s->data.t1.plus + s->data.t1.minus*(*max_nodes+*sources)] -= 1/s->data.t1.val;
      }
    } else if ( transient_analysis && s->data.type == C )  {
      if ( s->data.t1.plus < *max_nodes)
        matrix2[ s->data.t1.plus + s->data.t1.plus*(*max_nodes+*sources)] += s->data.t1.val;
      if ( s->data.t1.minus < *max_nodes )
        matrix2[ s->data.t1.minus + s->data.t1.minus*(*max_nodes+*sources)] += s->data.t1.val;
      if ( s->data.t1.minus < *max_nodes && s->data.t1.plus < *max_nodes ) {
        matrix2[ s->data.t1.minus + s->data.t1.plus*(*max_nodes+*sources)] -= s->data.t1.val;
        matrix2[ s->data.t1.plus + s->data.t1.minus*(*max_nodes+*sources)] -= s->data.t1.val;
      }
    } else if ( transient_analysis && s->data.type == L ) {
      matrix2[*max_nodes+*sources-inductors +s->data.t1.id 
        + (*max_nodes+*sources)*(*max_nodes+*sources-inductors+s->data.t1.id)] = - s->data.t1.val;
    }

    if ( s->data.type == V  || s->data.type == L ) {
      if ( s->data.t1.plus < *max_nodes ) {
        matrix[ *max_nodes + s->data.t1.id + (*max_nodes+*sources) * s->data.t1.plus ] = 1;
        matrix[ ( *max_nodes + s->data.t1.id ) * (*max_nodes+*sources) + s->data.t1.plus ] = 1;
      }
      if ( s->data.t1.minus < *max_nodes ) {
        matrix[ *max_nodes + s->data.t1.id + (*max_nodes+*sources) * s->data.t1.minus] = -1;
        matrix[ ( *max_nodes + s->data.t1.id ) * (*max_nodes+*sources) + s->data.t1.minus] = -1;
      }
    }
  }

  FILE* mna = fopen("mna_analysis", "w");

  printf("MNA matrix: see file \"mna_analysis\"\n" );

  for (y= 0; y < *max_nodes+*sources; y++ ) {
    /*if ( y == *max_nodes )
      fprintf(mna, "\n");*/
    for ( x=0; x < *max_nodes+*sources; x++ ) {
      /*if ( x == *max_nodes )
        fprintf(mna, "  ");*/
      fprintf(mna, "%10g", matrix[x + y*(*max_nodes+*sources)] );

    }
    fprintf(mna, "\n");
  }
  fclose(mna);

  if ( transient_analysis) { 
    mna = fopen("mna_analysis_transient", "w");

    printf("MNA matrix: see file \"mna_analysis_transient\"\n" );

    for (y= 0; y < *max_nodes+*sources; y++ ) {
      /*if ( y == *max_nodes )
        fprintf(mna, "\n");*/
      for ( x=0; x < *max_nodes+*sources; x++ ) {
        /*if ( x == *max_nodes )
          fprintf(mna, "  ");*/
        fprintf(mna, "%10g", matrix2[x + y*(*max_nodes+*sources)] );

      }
      fprintf(mna, "\n");
    }
    fclose(mna);

  }
}


int calculate_RHS(struct components_t *circuit,int max_nodes,int sources, double *RHS, double t){
  int y;
  struct components_t *s;
  double temp = 0;

  for ( y=0; y< max_nodes + sources; y++ )
    RHS[y] = 0;

  for (s=circuit; s!=NULL; s=s->next) {
    if ( s->data.type == I ) {
      if ( t >= 0 )
        temp = calculate_ac(s->data.t1.transient, t);

      if ( s->data.t1.plus != max_nodes )
        RHS[ s->data.t1.plus ] += s->data.t1.val + temp;
      else if ( s->data.t1.minus != max_nodes )
        RHS[ s->data.t1.minus] -= s->data.t1.val - temp;
      else {
        printf("Vraxukuklwma sthn phgh reumatos panw sth geiwsh\n");
        exit(0);
      }
      

    }
    else if ( s->data.type == V && s->data.t1.is_ground == 0 ) {
      if ( t >= 0 )
        temp = calculate_ac(s->data.t1.transient, t);
      RHS[ max_nodes + s->data.t1.id ] = s->data.t1.val + temp;
    }
  }

  printf("RHS: \n");

  for (y=0; y< max_nodes + sources ;y++ )	
    printf("%7g\n", RHS[y]);
  return 0;
}


/* Metetrepse ta indices se diadoxika */
int circuit_rename_nodes(struct components_t *circuit, struct instruction_t *instr, int element_types[], int **renamed_nodes, int *max_nodes)
{
  int *indices = NULL;
  int *voltage_sources = NULL, num_voltage_sources=0;
  int *current_sources = NULL, num_current_sources=0;
  int num_indices = 0, i,j;
  struct components_t *s;

  element_types[typeV] = element_types[typeI] = element_types[typeR] = element_types[typeC]
    = element_types[typeL] = element_types[typeD] = element_types[typeQ] = element_types[typeM]
    = 0;


  struct components_t *ground = NULL;
  struct instruction_t *p;


  for (s=circuit; s!=NULL; s=s->next) {
    switch( s->data.type ) {
      case V:
        if ( s->data.t1.val == 0 )
          ground = s;
        else {
          voltage_sources = (int*) realloc(voltage_sources, sizeof(int)*(num_voltage_sources+1) );
          voltage_sources[ num_voltage_sources++ ] = s->data.t1.id;
          s->data.t1.original_id = s->data.t1.id;
          s->data.t1.id = element_types[typeV]++;
        }
        break;
      case I:
        current_sources =(int*)realloc(current_sources, sizeof(int)* ( num_current_sources+1));
        current_sources[ num_current_sources++ ] = s->data.t1.id;
        s->data.t1.original_id = s->data.t1.id;
        s->data.t1.id = element_types[typeI]++;
        break;
      case R:
        s->data.t1.id = element_types[typeR]++;
        break;
      case L:
        s->data.t1.id = element_types[typeL]++;
        break;
      case C:
        s->data.t1.id = element_types[typeC]++;
        break;
      case D:
        s->data.t2.id = element_types[typeD]++;
        break;
      case M:
        s->data.t3.id = element_types[typeM]++;
        break;
      case Q:
        s->data.t3.id = element_types[typeQ]++;
        break;
      default:
        printf("Unknown element type in rename (%d)\n", s->data.type);
        continue;
    }

    switch ( s->data.type ) {
      case V: case I: case R: case L: case C:
        for (i=0; i<num_indices; i++)
          if ( indices[i] == s->data.t1.plus ) {
            s->data.t1.plus = i;
            break;
          }

        if ( i == num_indices ) { // paei na pei oti den to brhka mesa sto indices
          indices = (int*) realloc(indices, sizeof(int) * (++num_indices));
          indices[num_indices-1] = s->data.t1.plus;
          s->data.t1.plus = num_indices-1;
        }

        for (i=0; i<num_indices; i++)
          if ( indices[i] == s->data.t1.minus) {
            s->data.t1.minus = i;
            break;
          }

        if ( i == num_indices ) { // paei na pei oti den to brhka mesa sto indices
          indices = (int*) realloc(indices, sizeof(int) * (++num_indices));
          indices[num_indices-1] = s->data.t1.minus;
          s->data.t1.minus = num_indices-1;
        }
        break;

      default:
        printf("[#] Den exei ginei akoma gia ta upoloipa sto indices mesa sthn rename\n");
        return 0;
        break;
    }
  }

  if ( ground == NULL ) 
  {
    printf("To do8en kuklwma den exei geiwsh\n");
    exit(0);
  }

  if ( ground != NULL ) {
    ground->data.t1.id = element_types[typeV];
    int ground_node = ground->data.t1.plus;

    int temp = indices[ground_node];
    indices[ground_node] = indices[num_indices-1];
    indices[num_indices-1] = temp;

    for (s=circuit; s!=NULL; s=s->next) {
      switch( s->data.type ) {
        case V:
        case I:
        case R:
        case C:
        case L:
          if ( s->data.t1.plus == ground_node )
            s->data.t1.plus = num_indices-1;
          else if ( s->data.t1.plus == num_indices-1 )
            s->data.t1.plus = ground_node;
          if ( s->data.t1.minus == ground_node)
            s->data.t1.minus = num_indices-1;
          else if ( s->data.t1.minus == num_indices-1 )
            s->data.t1.minus = ground_node;
          break;
        case M:
          if ( s->data.t3.d == ground_node )
            s->data.t3.d = num_indices-1;
          else if ( s->data.t3.d == num_indices-1)
            s->data.t3.d = ground_node;

          if ( s->data.t3.g == ground_node )
            s->data.t3.g = num_indices-1;
          else if ( s->data.t3.g == num_indices-1)
            s->data.t3.g = ground_node;

          if ( s->data.t3.s == ground_node )
            s->data.t3.s = num_indices-1;
          else if ( s->data.t3.s == num_indices-1)
            s->data.t3.s = ground_node;

          if ( s->data.t3.b == ground_node )
            s->data.t3.b = num_indices-1;
          else if ( s->data.t3.b == num_indices-1)
            s->data.t3.b = ground_node;

          break;

        case Q:
          if ( s->data.t4.c == ground_node )
            s->data.t4.c = num_indices-1;
          else if ( s->data.t4.c == num_indices-1)
            s->data.t4.c = ground_node;

          if ( s->data.t4.b == ground_node )
            s->data.t4.b = num_indices-1;
          else if ( s->data.t4.b == num_indices-1)
            s->data.t4.b = ground_node;

          if ( s->data.t4.e == ground_node )
            s->data.t4.e = num_indices-1;
          else if ( s->data.t4.e == num_indices-1)
            s->data.t4.e = ground_node;
          break;

        case D:
          if ( s->data.t2.plus == ground_node )
            s->data.t2.plus = num_indices-1;
          else if ( s->data.t2.plus == num_indices-1 )
            s->data.t2.plus = ground_node;
          if ( s->data.t2.minus == ground_node)
            s->data.t2.minus = num_indices-1;
          else if ( s->data.t2.minus == num_indices-1 )
            s->data.t2.minus = ground_node;

          break;
        default:
          printf("[-] Error sto rename den uposthrizetai o tupos akoma! (%d)\n", s->data.type);
          break;
      }
    }
    *max_nodes = num_indices-1;
    *renamed_nodes = indices;
  } else {
    *max_nodes = num_indices;
    *renamed_nodes = indices;
  }

  printf("Nodes that were renamed:\n");
  for (i=0; i<num_indices; i++ )
    printf("\t%d -> %d\n", indices[i], i);

  for (p=instr; p!=NULL; p=p->next) {
    switch(p->type) {
      case Dc:
        if ( p->dc.sourceType == Voltage ) {
          for (i=0; i<num_voltage_sources; i++ ) {
            if ( voltage_sources[i] == p->dc.source ) {
              p->dc.source = i;
              break;
            }
          }

          if ( i == num_voltage_sources ) { // auto paei na pei oti den bre8hke h phgh
            printf("[-] Could not find voltage source %d (for instruction DC)\n", p->dc.source);
            exit(0);
          }
        } else if ( p->dc.sourceType == Current ) {
          for (i=0; i<num_current_sources; i++ ) {
            if ( current_sources[i] == p->dc.source ) {
              p->dc.source = i;
              break;
            }
          }

          if ( i == num_current_sources ) { // auto paei na pei oti den bre8hke h phgh
            printf("[-] Could not find current source %d (for instruction DC)\n", p->dc.source);
            exit(0);
          }
        }

        break;

      case Plot:
        for ( i=0; i< p->plot.num; i++ ) {
          for (j=0; j<num_indices; j++ ) {
            if ( indices[j] == p->plot.list[i] ) {
              if ( ground && j == num_indices-1 ) {
                printf("[#] Will not report Ground's voltage\n");
                fclose(p->plot.output[i]);

                for (j=i; j<p->plot.num-1; j++ ) {
                  p->plot.list[j] = p->plot.list[j+1];
                  p->plot.output[j] = p->plot.output[j+1];
                }
                p->plot.num--;
                p->plot.list = (int*) realloc(p->plot.list, sizeof(int) * ( p->plot.num));
                p->plot.output = (FILE**) realloc(p->plot.output, sizeof(FILE*) * ( p->plot.num));
                i--;
              } else {
                printf("plot ( was %d turned to %d )\n", p->plot.list[i], j);
                p->plot.list[i] = j;
              }
              break;
            }
          }

          if ( j == num_indices ) {

            printf("[-] Plot cannot report %d's voltage because it does not exist\n", p->plot.list[i]);
            exit(0);
          }
        }
        break;

      default:
        printf("[#] Rename Nodes --> Rename VoltageSources in instructions ( UNKNOWN INSTRUCTION %d )\n", p->type);
        break;
    }
  }

  if ( voltage_sources)
    free(voltage_sources);
  if (current_sources)
    free(current_sources);

  if ( ground )
    return num_indices-1;
  else
    return num_indices;
}

void solve(double *L, double *U, double *temp, double *result,
    double *RHS,
    int *P, int max_nodes, int sources, double t)
{
  calculate_RHS(g_components,max_nodes,sources,RHS, t);
  forward_substitution(L, RHS, temp ,P, max_nodes + sources);
  backward_substitution(U, temp, result, max_nodes+sources);
}

void print_help(char *path)
{
  char *file;
  file = strrchr(path, '/');

  if ( file == NULL )
    file = path;
  else
    file ++;

  printf("Usage: %s SOURCE_FILE\n", file);
}


int instruction_dc_sparse(struct instruction_t *instr, int max_nodes, int sources, int renamed_nodes[], 
    double *RHS, css *S, csn *N, cs* MNA, double *m, double *result)
{

  double begin;
  double end;
  double step;
  double dummy;
  int i;
  int original_source_val;
  ;
  struct components_t *s;
  struct instruction_t *ptr = g_instructions;
  int j;


  begin = instr->dc.begin;
  end = instr->dc.end;
  step = instr->dc.inc;

  if(instr->dc.sourceType == Voltage){
    for (s=g_components;s!=NULL; s=s->next) {
      if( s->data.type == V && s->data.t1.id == instr->dc.source){
        i = 0;
        original_source_val = s->data.t1.val;
        for(dummy = begin ; dummy <= end ;i++, dummy = dummy+step){

          s->data.t1.val = dummy;
          printf("Solving for Voltage Source value %g\n",dummy);
          calculate_RHS(g_components,max_nodes,sources,RHS, -1);
          if ( iter_type == NoIter ) {
            if ( spd_flag == 0 ) 
              cs_lusol(S, N, RHS, result, (max_nodes+sources));
            else
              cs_cholsol(S,N, RHS, result, (max_nodes+sources));
          }else {
            memset(result, 0, sizeof(double) * ( max_nodes+sources));
            if ( iter_type == CG ) {
              conjugate_sparse(MNA, result , RHS, m, itol, max_nodes+sources);
            } else if ( iter_type == BiCG ) {
              biconjugate_sparse(MNA, result , RHS, m, itol, max_nodes+sources);
            }
          }

          printf("Result:\n");
          print_array(result,max_nodes+sources);

          for ( ptr = g_instructions; ptr!=NULL; ptr=ptr->next) {

            if ( ptr->type == Plot ) {
              for (j=0; j<ptr->plot.num; j++ )
                fprintf( ptr->plot.output[j], "%G %g\n", 
                    dummy, result[ptr->plot.list[j]]);

            }

          }
        }
        s->data.t1.val = original_source_val;
      } 
    }
  } else if  (instr->dc.sourceType == Current ) {
    for (s=g_components;s!=NULL; s=s->next) {
      if( s->data.type == I && s->data.t1.id == instr->dc.source){
        i = 0;
        original_source_val = s->data.t1.val;
        for(dummy = begin ; dummy <= end ;i++, dummy = dummy+step){

          s->data.t1.val = dummy;
          printf("Solve for Current Source value %g\n",dummy);

          calculate_RHS(g_components,max_nodes,sources,RHS, -1);
          if ( iter_type == NoIter ) {
            if ( spd_flag == 0 ) 
              cs_lusol(S, N, RHS, result, (max_nodes+sources));
            else
              cs_cholsol(S,N, RHS, result, (max_nodes+sources));
          }else {
            memset(result, 0, sizeof(double) * ( max_nodes+sources));
            if ( iter_type == CG ) {
              conjugate_sparse(MNA, result , RHS, m, itol, max_nodes+sources);
            } else if ( iter_type == BiCG ) {
              biconjugate_sparse(MNA, result , RHS, m, itol, max_nodes+sources);
            }
          }

          printf("Result:\n");
          print_array(result,max_nodes+sources);

          for (ptr = g_instructions; ptr!=NULL; ptr=ptr->next) {

            if ( ptr->type == Plot ) {
              for (j=0; j<ptr->plot.num; j++ )
                fprintf( ptr->plot.output[j], "%G %g\n", 
                    dummy, result[ptr->plot.list[j]]);

            }

          }}
          s->data.t1.val = original_source_val;
      } 
    }
  }

  /*for ( i=0; i < max_nodes+sources; i++ ) {
    printf("%d -- %d (%g)\n", S->q[i], N->pinv[i], result[i]);
    }*/
  return 0;

}


int instruction_dc(struct instruction_t *instr, int max_nodes, int sources, int renamed_nodes[], 
    double *MNA, double *RHS, double *L, double *U, double *m, int *P,
    double *temp, double *result)
{
  double begin;
  double end;
  double step;
  double dummy;
  int i;
  int original_source_val;
  struct components_t *s;
  struct instruction_t *ptr = g_instructions;
  int j;

  begin = instr->dc.begin;
  end = instr->dc.end;
  step = instr->dc.inc;


  if(instr->dc.sourceType == Voltage){
    for (s=g_components;s!=NULL; s=s->next) {
      if( s->data.type == V && s->data.t1.id == instr->dc.source){
        i = 0;
        original_source_val = s->data.t1.val;
        for(dummy = begin ; dummy <= end ;i++, dummy = dummy+step){

          s->data.t1.val = dummy;
          printf("Solving for Voltage Source value %g\n",dummy);
          if ( iter_type == NoIter )
            solve(L,U,temp,result,RHS,P,max_nodes,sources, -1);
          else {
            calculate_RHS(g_components,max_nodes,sources,RHS, -1);
            memset(result, 0, sizeof(double) * ( max_nodes+sources));
            if ( iter_type == CG ) {
              conjugate(MNA, result , RHS, m, itol, max_nodes+sources);
            } else if ( iter_type == BiCG ) {
              biconjugate(MNA, result , RHS, m, itol, max_nodes+sources);
            }
          }
          printf("Result:\n");
          print_array(result,max_nodes+sources);

          for ( ptr = g_instructions; ptr!=NULL; ptr=ptr->next) {

            if ( ptr->type == Plot ) {
              for (j=0; j<ptr->plot.num; j++ )
                fprintf( ptr->plot.output[j], "%G %g\n", 
                    dummy, result[ptr->plot.list[j]]);

            }

          }
        }
        s->data.t1.val = original_source_val;
      } 
    }
  } else if  (instr->dc.sourceType == Current ) {
    for (s=g_components;s!=NULL; s=s->next) {
      if( s->data.type == I && s->data.t1.id == instr->dc.source){
        i = 0;
        original_source_val = s->data.t1.val;
        for(dummy = begin ; dummy <= end ;i++, dummy = dummy+step){

          s->data.t1.val = dummy;
          printf("Solve for Current Source value %g\n",dummy);


          if ( iter_type == NoIter ) {
            solve(L,U,temp,result,RHS,P,max_nodes,sources,-1);
          } else {
            calculate_RHS(g_components,max_nodes,sources,RHS, -1);
            memset(result, 0, sizeof(double) * ( max_nodes+sources));
            if ( iter_type == CG ) {
              conjugate(MNA, result , RHS, m, itol, max_nodes+sources);
            } else if ( iter_type == BiCG ) {
              biconjugate(MNA, result , RHS, m, itol, max_nodes+sources);
            }
          }
          printf("Result:\n");
          print_array(result,max_nodes+sources);

          for (ptr = g_instructions; ptr!=NULL; ptr=ptr->next) {

            if ( ptr->type == Plot ) {
              for (j=0; j<ptr->plot.num; j++ )
                fprintf( ptr->plot.output[j], "%G %g\n", 
                    dummy, result[ptr->plot.list[j]]);

            }

          }}
          s->data.t1.val = original_source_val;
      } 
    }
  }

  return 0;
}


int execute_instructions(double *MNA_G, double *MNA_C, cs *MNA_sparse_G, cs *MNA_sparse_C, int max_nodes, int sources, int *renamed_nodes, int stoixeia[])
{
  double *RHS = NULL;
  double *L=NULL,*U=NULL,*result=NULL, *m=NULL, *temp=NULL;
  int *P=NULL;
  int i;
  struct instruction_t *instr;

  // Gia tous sparse pinakes
  cs *MNA_compressed_G = NULL, *MNA_compressed_C = NULL;
  css *S =  NULL;
  csn *N = NULL;

  instr = g_instructions;

  RHS = (double*) calloc(max_nodes+sources, sizeof(double));
  result = (double*) calloc((max_nodes+sources), sizeof(double));
  dc_point = (double*) calloc((max_nodes+sources), sizeof(double));

  if ( use_sparse == 0 ) {
    if ( iter_type == NoIter ) {
      L = (double*) calloc((max_nodes+sources)*(max_nodes+sources), sizeof(double));
      U = (double*) calloc((max_nodes+sources)*(max_nodes+sources), sizeof(double));
      P = (int*) calloc((max_nodes+sources), sizeof(int));
      temp = (double* ) calloc( (max_nodes+sources) * (max_nodes+sources), sizeof(double));
      for( i = 0 ; i < max_nodes + sources; i++ )
        P[i] = i;
      if ( spd_flag==0) {
        LU_decomposition(MNA_G, L, U, P, max_nodes + sources );
      } else {
        if ( stoixeia[typeV] ) {
          printf("[-] Den prepei na uparxoun phges Tashs\n");
          return 0;
        }

        if ( stoixeia[typeL] ) {
          printf("[-] Den prepei na uparxoun phnia\n");
          return 0;
        }

        if ( stoixeia[typeD] ) {
          printf("[-] Den prepei na uparxoun diodoi\n");
          return 0;
        }

        if ( stoixeia[typeQ] ) {
          printf("[-] Den prepei na uparxoun transistor BJT \n");
          return 0;
        }

        if ( stoixeia[typeM] ) {
          printf("[-] Den prepei na uparxoun transistor CMOS\n");
          return 0;
        }


        if ( !Choleski_LDU_Decomposition(MNA_G, L, max_nodes + sources ) ) {
          printf("[-] Negative value on array L\n");
          exit(0);
        }

        calculate_transpose(L, U, max_nodes + sources );
      }

      solve(L,U,temp,dc_point,RHS,P,max_nodes,sources, -1);
      printf("Circuit Solution\n");
      print_array(dc_point, max_nodes+sources);

    } else {
      m = (double*) malloc(sizeof(double) * (max_nodes+sources));

      for (i=0; i<max_nodes+sources; i++ ) 
        m[i] = MNA_G[i*(max_nodes+sources)+i];

      calculate_RHS(g_components,max_nodes,sources,RHS, -1);
      biconjugate(MNA_G, dc_point, RHS, m, itol, max_nodes+sources);
      printf("Circuit Solution\n");
      print_array(dc_point, max_nodes+sources);
    }
  } else {
    MNA_compressed_G = cs_compress(MNA_sparse_G);
    cs_spfree(MNA_sparse_G);
    calculate_RHS(g_components,max_nodes,sources,RHS,-1);
    if ( iter_type == NoIter ) {
      // Sparse Matrices

      if ( spd_flag == 0 ) {
        // edw exw LU
        S = cs_sqr (2, MNA_compressed_G, 0) ;              /* ordering and symbolic analysis */
        N = cs_lu (MNA_compressed_G, S, 1) ;                 /* numeric LU factorization */
        cs_spfree(MNA_compressed_G);
        MNA_compressed_G = NULL;
        cs_lusol(S, N, RHS, dc_point, (max_nodes+sources));
        printf("Circuit Solution\n");
        print_array(dc_point, max_nodes+sources);

      } else {
        // edw exw cholesky
        S = cs_schol(1,MNA_compressed_G);
        N = cs_chol(MNA_compressed_G,S);
        cs_spfree(MNA_compressed_G);
        MNA_compressed_G = NULL;
        cs_cholsol(S, N, RHS, dc_point, (max_nodes+sources));
        printf("Circuit Solution\n");
        print_array(dc_point, max_nodes+sources);
      }
    } else {
      m = (double*) malloc(sizeof(double) * (max_nodes+sources));

      for (i=0; i<max_nodes+sources; i++ ) 
        m[i] = cs_atxy(MNA_compressed_G, i, i );

      biconjugate_sparse(MNA_compressed_G, dc_point, RHS, m, itol, max_nodes+sources);
      printf("Circuit Solution\n");
      print_array(dc_point, max_nodes+sources);
    }

  }


  while ( instr ) {
    if(instr->type == Dc) {
      if ( use_sparse == 0 ) {
        instruction_dc(instr,max_nodes, sources, renamed_nodes, 
            MNA_G, RHS, L, U, m, P, temp, result);
      } else {
        instruction_dc_sparse(instr, max_nodes, sources, renamed_nodes,
            RHS, S, N, MNA_compressed_G,m, result);
      }
    }
    instr = instr->next;
  }

  if (dc_point )
    free(dc_point);

  if (  S  )
    cs_sfree(S);
  if ( N ) 
    cs_nfree(N);
  if ( MNA_compressed_G )
    cs_spfree(MNA_compressed_G);

  if ( MNA_compressed_C )
    cs_spfree(MNA_compressed_C);

  if ( m ) free(m);
  if ( L ) free(L);
  if ( U ) free(U);
  if ( P ) free(P);
  if ( temp ) free(temp);
  if ( result) free(result);
  if ( MNA_G) free(MNA_G);
  if ( RHS ) free(RHS);
  if ( renamed_nodes ) free(renamed_nodes);

  return 0;
}


int main(int argc, char* argv[])
{
  int ret;
  double *MNA_G=NULL, *MNA_C=NULL;
  cs *MNA_sparse_G=NULL, *MNA_sparse_C = NULL;
  int max_nodes, sources, *renamed_nodes;
  int stoixeia[8] = { 0,0,0,0,0,0,0,0 };
  struct option_t *o;
  use_sparse = 0 ;
  g_instructions = NULL;
  g_components = NULL;
  g_options = NULL;

  if ( argc != 2 ) {
    print_help(argv[0]);
    return 0;
  }

  yyin = fopen(argv[1], "r");

  if ( yyin == NULL ) {
    printf("[-] Could not open input file \"%s\"\n", argv[1]);
    return 1;
  }

  ret = yyparse();
  fclose(yyin);
  if ( ret == 0 )
    printf("[+] No errors\n");
  else
    return 1;

  circuit_print(g_components);
  instructions_print(g_instructions);

  for (o=g_options; o!=NULL; o=o->next) {
    switch ( o->type ) {
      case SPD:
        spd_flag = 1;
        break;

      case ITER:
        iter_type = o->iter_type;
        break;

      case ITOL:
        itol = o->itol;
        break;

      case SPARSE:
        use_sparse = 1;
        break;
    }
  }


  if ( use_sparse==0 )
    circuit_mna(g_components,&MNA_G, &MNA_C,&max_nodes,&sources, stoixeia, &renamed_nodes);
  else
    circuit_mna_sparse(g_components,&MNA_sparse_G, &MNA_sparse_C,&max_nodes,
      &sources, stoixeia, &renamed_nodes);

  execute_instructions(MNA_G, MNA_C, MNA_sparse_G, MNA_sparse_C, max_nodes,
      sources, renamed_nodes, stoixeia);

  circuit_cleanup(g_components);
  instructions_cleanup(g_instructions);
  options_cleanup(g_options);

  yylex_destroy();
  return 0;
}

