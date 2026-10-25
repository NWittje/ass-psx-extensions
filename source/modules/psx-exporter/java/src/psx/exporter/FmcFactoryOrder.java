package psx.exporter;

import java.util.*;

/** Constructor dependencies of the qualified native profile. Ownership cycles
 * are allowed; constructor-required cycles are rejected before any factory runs.
 * Topological ordering is linear in nodes and declared dependency edges;
 * constructor-field lookup uses the sorted cells. All work is on a worker. */
final class FmcFactoryOrder {
    record Rule(int type,int[] constructorReferences){}
    private FmcFactoryOrder(){}
    private static void require(boolean value,String reason){if(!value)throw new IllegalArgumentException("FMC factory "+reason);}
    static int[] order(FmcGraph.Graph graph,List<Rule> rules){
        require(graph.nodes().length>0&&graph.nodes().length<=FmcGraph.MAX_NODES,"graph count");
        var byType=new HashMap<Integer,int[]>();
        for(Rule rule:rules){require(rule.type>0&&byType.put(rule.type,rule.constructorReferences)==null,"duplicate type");int previous=0;for(int field:rule.constructorReferences){require(field>previous,"field order");previous=field;}}
        int count=graph.nodes().length,edges=0;int[] needed=new int[count],heads=new int[count];Arrays.fill(heads,-1);
        for(var node:graph.nodes()){
            require(node.id()>0&&node.id()<=count&&graph.nodes()[node.id()-1]==node,"node identity");
            int[] refs=byType.get(node.type());
            if(node.kind()!=1){require(refs==null||refs.length==0,"array constructor references");continue;}
            require(refs!=null,"missing record factory");
            for(int field:refs){int reference=reference(node,field);if(reference>0){require(reference<=count,"missing constructor object");require(edges<FmcGraph.MAX_CELLS,"dependency capacity");edges++;needed[node.id()-1]++;}}
        }
        int[] targets=new int[edges],next=new int[edges];int edge=0;
        for(var node:graph.nodes())if(node.kind()==1)for(int field:byType.get(node.type())){
            int ref=reference(node,field);if(ref>0){int source=ref-1;targets[edge]=node.id()-1;next[edge]=heads[source];heads[source]=edge++;}
        }
        int[] order=new int[count];int start=0,end=0;for(int i=0;i<count;i++)if(needed[i]==0)order[end++]=i;
        while(start<end){int source=order[start++];for(int e=heads[source];e>=0;e=next[e]){int target=targets[e];if(--needed[target]==0)order[end++]=target;}}
        require(end==count,"constructor dependency cycle");return order;
    }
    private static int reference(FmcGraph.Node node,int field){
        int low=0,high=node.cells().length-1;
        while(low<=high){int middle=(low+high)>>>1;var cell=node.cells()[middle];if(cell.id()<field)low=middle+1;else if(cell.id()>field)high=middle-1;else{require(cell.kind()==9&&(cell.bits()&0xffffffff00000000L)==0,"constructor reference kind");return (int)cell.bits();}}
        throw new IllegalArgumentException("FMC factory missing constructor field");
    }
}
